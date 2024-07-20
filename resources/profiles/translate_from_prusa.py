import re

filename_in = "MakerGear_init.ini"
filename_out = "MakerGear_out.ini"

in_stream = open(filename_in, mode="r", encoding="utf-8");
lines = in_stream.read().splitlines();
in_stream.close();
file_out_stream = open(filename_out, mode="w", encoding="utf-8");
brim_type = "both";
for line in lines:
# xy compensaiton
	line = re.sub(
		r"\[first_layer_temperature]",
		"{first_layer_temperature+extruder_temperature_offset}", 
		line
	);
	line = re.sub(
		r"{first_layer_temperature}",
		"{first_layer_temperature+extruder_temperature_offset}", 
		line
	);
	line = re.sub(
		r"^elefant_foot_compensation = ([0-9]\.[0-9]*)",
		"first_layer_size_compensation = -\g<1>", 
		line
	);
# bridges
	line = re.sub(
		r"^bridge_flow_ratio = ([0-1])\.([0-9][0-9])$",
		"bridge_flow_ratio = \g<1>\g<2>%",
		line
	);
	line = re.sub(
		r"^bridge_flow_ratio = ([0-1])\.([0-9])$",
		"bridge_flow_ratio = \g<1>\g<2>0%",
		line
	);
	line = re.sub(
		r"^bridge_flow_ratio = 1$",
		"bridge_flow_ratio = 100%",
		line
	);
	line = re.sub(
		r"^thick_bridges = 0$",
		"bridge_type = flow\nbridge_overlap_min = 60%\nbridge_overlap = 75%",
		line
	);
	line = re.sub(
		r"^thick_bridges = 1$",
		"bridge_type = nozzle\nbridge_overlap_min = 80%\nbridge_overlap = 95%",
		line
	);
# brim
	if line.startswith("brim_type ="):
		brim_type = re.sub(r"^brim_type = (.*)$", "\g<1>", line);
		continue;
	if line.startswith("brim_width ="):
		if brim_type == "inner_only":
			line = re.sub(r"^brim_width = (.*)$", "brim_width_interior = \g<1>", line);
		elif brim_type == "no_brim":
			line = "brim_width = 0\nbrim_width_interior = 0";
		elif brim_type == "outer_and_inner":
			line = re.sub(r"^brim_width = (.*)$", "brim_width = \g<1>;brim_width_interior = \g<1>", line);
#others
	line = re.sub(
		r"^first_layer_speed = ([0-9.]+)",
		"first_layer_speed = \g<1>\nfirst_layer_min_speed = 0\nfirst_layer_infill_speed = 100%",
		line
	);
	line = re.sub(
		r"^resolution = 0$",
		"resolution = 0.0125",
		line
	);
	line = re.sub(
		r"support_material_contact_distance = 0$",
		"support_material_contact_distance_type = none",
		line
	);
	line = re.sub(
		r"^(extrusion_width = .*)$",
		"\g<1>\nextrusion_spacing =\nperimeter_extrusion_spacing =\nexternal_perimeter_extrusion_spacing =\nfirst_layer_extrusion_spacing =\ninfill_extrusion_spacing =\nsolid_infill_extrusion_spacing =\ntop_infill_extrusion_spacing =",
		line
	);
	line = re.sub(
		r"^overhangs = 0$",
		"overhangs_width_speed = 0",
		line
	);
	line = re.sub(
		r"^seam_position = nearest$",
		"seam_position = cost\nseam_angle_cost=50%\nseam_travel_cost=50%",
		line
	);
	line = re.sub(
		r"^(thumbnails = 1)$",
		"\g<1>\nthumbnails_with_bed = 1",
		line
	);
	# first_layer_height in % of layer height vs % of nozzle diameter
	line = re.sub(
		r"^first_layer_height = ([0-9]+%)$",
		"# From prusa: first_layer_height should be \g<1> of the first_layer_height\nfirst_layer_height = 50%",
		line
	);
	line = re.sub(
		r"^cooling = 0$",
		"# From prusa: cooling = 0, so set the default fan speed to 0. Delete if already set\ndefault_fan_speed = 0",
		line
	);
	line = re.sub(
		r"^cooling = 1$",
		"",
		line
	);
	# note: if min_fan_speed is already set, you may have to delete manually one.
	line = re.sub(
		r"^min_fan_speed = ([0-9]+)$",
		"# From prusa: min_fan_speed is now a printer setting. please copy it where the printer is. Currently letting here as default_fan_speed.\ndefault_fan_speed = \g<1>",
		line
	);
	
	
	
	if(line != "overhangs = 1"):
		file_out_stream.write(line);
		file_out_stream.write("\n");
file_out_stream.close();








