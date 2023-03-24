# UI layouts wiki

UI layout can be selected in the `configuration->preferences->GUI->settings layout UI and colors`.
In this box, you can select the layout you want to use, each one has its directory in ui_layout in your configuration folder (use help->show configuration folder to get to it).
Note: The configuration folder is created at the first launch of superslicer.

## add a new layout

To add a new layout, copy-paste the `default` directory, and rename it to your new name ('newlayout' for this wiki)
Go inside this directory.

Edit the version.ini file: 
 * the name will be displayed in the preference combobox and the description will be used as the tooltip.
Then, you can edit teh ui file to change the layout of each tab (print, filament and printer).
Note that each setting has to be in its tab, it can't change. That's why there is print_temperature, as the temperature field is linked to the filament, and can't be in a print setting.
But inside a tab, you have full control.

# How to customize a setting UI

## How it works
The software will search for each tab the needed ui files
* for fff printers:
  * "print.ui" for the Print Settings tab
  * "filament.ui" for the Filament Settings tab
  * "printer.ui" for the Printer Settings tab
* for slaprinters:
  * "sla_print.ui" for the Print Settings tab
  * "sla_material.ui" for the Filament Settings tab
  * "sla_printer.ui" for the Printer Settings tab
If a ui file isn't here, it will build the tab with the default (harcoded) layout.
## syntax
The tree is composed of page, group, lines and settings
A group has to be inside a page.
A line has to be inside a group.
A setting has to be inside a group or a line.
Each object has parameters
### syntax of each object
STR represent a label that can conatins any character but ':', leading and trailing space and tabs are removed.
INT represent an integer
parameters that are inside [] are optionals
each parameter is separated by ':'
* Page: 
	`page[:idx]:STR:STR`
	* first STR is for the label and the second for the icon, with or without the .svg / .png
	* idx: append the index of the page (for extruder ui) to the name
* Group: 
	`group[:no_title][:title_width$INT][:label_width$INT][:sidetext_width$INT][:EVENT][:id$INT][:idx]:STR`
	* `EVENT` can be extruders_count_event if the group contains extruders_count and is a printer tab ; silent_mode_event if the group contains silent_mode and is a printer tab ; material_density_event if the group contains material_density.
	* `title_width$INT` is used to set the size of the left column, where titles are draw.
	* `label_width$INT` is used to set the size of the labels on lines.
	* `sidetext_width$INT` is used to set the size of the suffix label (see sidetext in setting).
	* `no_title` is used to remove the left column, where titles are draw.
	* `no_search` to not add this group's widgets into the search tool.
* Line:
	`line:STR*`
* setting:
	`setting[label$STR][label_width$INT][:full_label][:full_width][:sidetext$STR][sidetext_width$INT][:simple|advanced|expert][:width$INT][:height$INT][:id$INT]:STR`
	* `STR`, the last parameter: the id name of the setting.
	* `label$STR`: to override the label by this new one (if it ends with '_' it won't have a ':' ; if empty it won't have a length).
	* `label_width$INT`: change the width of the label. Only works if it's in a line. Override the group one. -1 for auto.
	* `label_left`: Draw the label aligned to the left instead of the right.
	* `full_label$STR`: to override the full_label by this new one (full_label is used on modifiers).
	* `full_label`: to override the label by the "full one".
	* `tooltip$STR`: to override the tooltip. Note that you can write '\t' for a tabulation, '\n' for a new line, '\.' for a ':' and '\£' for a '$'.
	* `full_width`: to tell to create a field that span the full width.
	* `sidetext$STR`: the suffix at the right of the widget (like 'mm').
	* `sidetext_width$INT`: the suffix label length (override the group one). -1 for auto.
	* `max_literal$INT[%]`: if the user enter a value higher than that and it's a 'float or percent' field, then emit a pop-up to question if he doesn't forgot a '%'. If negative, it check if the value isn't lower than the absolute max_literal, instead of greater. If there is a '%' after the value, then it's multiplied by the biggest nozzle diameter.
	* `tags$STR[$STR]*`: to set the tags in which the setting can show up. Each tag has to be suffixed by a `$`. 
	* simple|advanced|expert: old way to set the tags: simple will set the tags to 'Simple', advanced will set the tags to 'Simple' and 'Advanced', and 'expert' will set the tags to 'Simple' 'Advanced' and 'Expert'.
	* `width$INT`: change the width of the field. Shouod work on most type of settings.
	* `height$INT`: change the height of the field. Don't works with every type of setting (mostly multilne text). Set to -1 to 'disable'.
	* `precision$INT`: number of digit after the dot displayed.
	* `url$STR`: the url to call when clicking on it.
	* `id $INT`: for setting only a single value of a setting array.
	* `idx`: for setting only a single value of a setting array, with the index of the page (for extruder ui page)
	* `script`: to tell that this setting doesn't exist and is defined in a script (see script section). You have to also define the type. They comes in two flavor: single value and array (Single values have to be used, but for filament and extruder tabs that require arrays):
		* `bool`: Single vlaue. Tell the sotfware it's a checkbox (boolean return value)
		* `int`: Single vlaue. Tell the sotfware it's a int entry field (int return value)
		* `float`: Single vlaue. Tell the sotfware it's a numeric entry field (float return value)
		* `percent`: Single vlaue. Tell the sotfware it's a percentage (float return value)
		* `float_or_percent`: Single vlaue. Tell the sotfware it's a numeric that can acept a % value (float return value)
		* `string`: Single vlaue. Tell the sotfware it's a numeric an entry field where you can enter a text(string return value)
		* `bools`: Array. Tell the sotfware it's a checkbox (boolean return value)
		* `ints`: Array. Tell the sotfware it's a int entry field (int return value)
		* `floats`: Array. Tell the sotfware it's a numeric entry field (float return value)
		* `percents`: Array. Tell the sotfware it's a percentage (float return value)
		* `floats_or_percents: mandatory`: Array. Tell the sotfware it's a numeric that can acept a % value (boolean return value)
		* `strings`: Array. Tell mandatory for a filament or an extruder value. The sotfware it's a numeric an entry field where you can enter a text(string return value)
		* `enum$STR$STR[$STR$STR]*`: tell the sotfware it's a combobox (string return value). It has to be followed by $name$label for each option you want to show.
	* Also, script may depends on normal fields. When a setting it depends is modified, the scripted widget will appear modified. And resetting a widget will reset all depending fields.
		* `depends$STR[$STR]*`: add the setting fields this scripted widget depends on. Each one has to be suffixed by a '$'

There is also special commands:
* `height:INT`: change the default height of settings. Don't works with every type of setting (mostly multilne text). Set to 0 or -1 to disable.
* `recommended_thin_wall_thickness_description`: create a text widget to explain recommended thin wall thickness (only in a fff print tab).
* `parent_preset_description`: create a text widget to explain parent preset.
* `cooling_description`: create a text widget to explain cooling (only in a filament tab).
* `volumetric_speed_description`: create a text widget to explain volumetric speed (only in a filament tab).
* `filament_ramming_parameters`: create a  widget for filament ramming.
* `filament_overrides_page`: create a page for overrides (only in a filament tab).
* `unregular_pages`: create needed special pages for a fff printer tab.
* `printhost`: create printhost settings for the group (only in a printer tab).
* `bed_shape`: create bed shape widget (only in a printer tab).
* `extruders_count`: create extruders_count setting (only in a fff printer tab).
* `logs`: activated logs.

### ui file syntax
trailing & leading tabs & spaces are removed, so you can indent as you want.
If the first character is '#', then this line is ignored
You can end page, group and line section by end_page, end_group but it's not mandatory as they do nothing. You have to use end_line because it indicates when the line end and you have to stop adding settings inside. Note that it's added automatically when line, group or page is called.

exemple:

    page:my page:my icon name
    	group:my group name
    		setting:label$Choose your base layer height, if you dare:layer_height
    		line:perimeters
    			settings:label$count:perimeters
    			settings:label$only one is spiral:spiral_vase
    		end_line
    	end_group
    end_page

# Scripts

## how to write a script

The file the script is written is a .as file. If a scripted widget is used in the print.ui file, then it has to be defined in the print.as file.

The language used is [angelscript](https://www.angelcode.com/angelscript). 

To write the functions for a scripted widget with a 'my_widget' name and a type 'int', then you have to write these methods:
* int my_widget_get()
* void my_widget_set(int value)

The first one is called when the gui need the value to display.
The second one is called when the user change the value in the gui.

At the start of the print.ui file, there is a little dictionnary with he methods available for the script to call.
It also contains the list of methods needed for all types (not only int).

You can also write a `void my_widget_reset()` if the default one (reset the settings from the depends property) isn't what's needed.

## frequent settings

The files freq_fff.ui and freq_sla.ui contains teh frequent settings.
Their layout is similar as other ui files. They still have to define a page and a group.
The group has to be `group:freq_settings_event:no_title:no_search:` to not interfer witht he rest of the gui.
If it use scripted widget, they have to be in the print.as file. The syntax is similar to c++/java.

## Script API: called by the slicer
Here are all the functions you need to define for your custom widget to work (for your type):
### type bool:
 * int **OPTNAME_get**() 
      Will return 1 if checked, 0 if unchecked and -1 if half-checked (not all os, will be unchecked if not available)
 * void **OPTNAME_set**(bool set)
###  type int:
 * int **OPTNAME_get**()
 * void **OPTNAME_set**(int set)
###  type float & percent:
 * float **OPTNAME_get**()
 * void **OPTNAME_set**(float set)
 ### type float_or_percent:
 * float **OPTNAME_get**(bool &out isPercent)
 * void **OPTNAME_set**(float set, bool isPercent)
###  type string:
 * void **OPTNAME_get**(string &out get)
 * void **OPTNAME_set**(string &in set)
###  type enum:
 * int **OPTNAME_get**(string &out enum_value)
      Only the return value is used unless it's out of bounds, then it tries to use the enum_value
 * void **OPTNAME_set**(string &in set_enum_value, int set_idx)
###  all type:
 * void **OPTNAME_reset**()
      Optional, called when clicked on the "reset" button.
      If not present, the software will call a reset on all fields from the 'depends' property.
## Script API: called by the script
 Here are listed all the functions to the slicer you can call from the script:
### to get the value of a real settings
These functions can be called everywhere.
 * bool  **get_bool**(string &in key)
 * int   **get_int**(string &in key)
    Can be used by type int and enum (return the index)
 * float **get_float**(string &in key)
    Can be used by type float, percent and flaot_or_percent
 * float **get_computed_float**(string &in key)
    Get the float computed value of the field. Useful if it's a floatOrPercent that is computable.
 * bool  **is_percent**(string &in key)
 * void  **get_string**(string &in key, string &out get_val)
    Can be used by type string and enum (return the enum_value, not the label)
    
### to set the value of real settings
These functions can only be called in a `set` or `reset` function. If you need to set a variable in a `get`, call ask_for_refresh() and write it in the set method.
 * void **set_bool**(string &in key, bool new_val)
 * void **set_int**(string &in key, int new_val)
    if an enum, it's the index in the c++ enum
 * void **set_float**(string &in key, float new_val)
    if a float_or_percent, unset the percent flag at the same time
 * void **set_percent**(string &in key, float new_val)
    if a float_or_percent, set the percent flag at the same time
 * void **set_string**(string &in key, string &in new_val))
    if an enum, it's one of the enum_value, as saved in a config file
 *  void **back_initial_value**(string &in key)
    revert the setting to the last saved value (same as a click on the reset arrow)
### others
  * void **ask_for_refresh**()
    ask for a OPTNAME_set() after the current OPTNAME_get(), to be able to set settings.

### to get/set the value of a custom variable
The first argument is the index of the tab setting: 0 for print settings, 1 for filament settings and 2 for printer settings.
Getters return true if the variable exists, false if it doesn't. If the variable doesn't exists, the 'out' variable isn't set.
 * bool **get_custom_bool**(int, string &in, bool &out)
 * void **set_custom_bool**(int, string &in, bool new_val)
 * bool **get_custom_int**(int, string &in, int &out)
 * void **set_custom_int**(int, string &in, int new_val)
 * bool **get_custom_float**(int, string &in, float &out)
 * void **set_custom_float**(int, string &in, float new_val)
 * bool **get_custom_string**(int, string &in, string &out)
 * void **set_custom_string**(int, string &in, string &in new_val)
 * void **back_custom_initial_value**(int, string &in)
To remove a custom variable, use `set_custom_string` with an empty string as `new_val`

### to print on the console, for debugging
 * void **print**(string &out)
 * void **print_float**(float)
