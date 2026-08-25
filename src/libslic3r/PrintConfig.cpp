///|/ Copyright (c) Prusa Research 2016 - 2023 Vojtěch Bubník @bubnikv, Lukáš Matěna @lukasmatena, Lukáš Hejl @hejllukas, Tomáš Mészáros @tamasmeszaros, Oleksandra Iushchenko @YuSanka, Pavel Mikuš @Godrak, David Kocík @kocikdav, Enrico Turri @enricoturri1966, Filip Sykala @Jony01, Vojtěch Král @vojtechkral
///|/ Copyright (c) 2023 Pedro Lamas @PedroLamas
///|/ Copyright (c) 2023 Mimoja @Mimoja
///|/ Copyright (c) 2020 - 2021 Sergey Kovalev @RandoMan70
///|/ Copyright (c) 2021 Niall Sheridan @nsheridan
///|/ Copyright (c) 2021 Martin Budden
///|/ Copyright (c) 2021 Ilya @xorza
///|/ Copyright (c) 2020 Paul Arden @ardenpm
///|/ Copyright (c) 2020 rongith
///|/ Copyright (c) 2019 Spencer Owen @spuder
///|/ Copyright (c) 2019 Stephan Reichhelm @stephanr
///|/ Copyright (c) 2018 Martin Loidl @LoidlM
///|/ Copyright (c) SuperSlicer 2018 Remi Durand @supermerill
///|/ Copyright (c) 2016 - 2017 Joseph Lenox @lordofhyphens
///|/ Copyright (c) Slic3r 2013 - 2016 Alessandro Ranellucci @alranel
///|/ Copyright (c) 2016 Vanessa Ezekowitz @VanessaE
///|/ Copyright (c) 2015 Alexander Rössler @machinekoder
///|/ Copyright (c) 2014 Petr Ledvina @ledvinap
///|/
///|/ PrusaSlicer is released under the terms of the AGPLv3 or higher
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/

#include "PrintConfig.hpp"
#include "FFFPrintConfig.hpp"
#include "SLA/SLAPrintConfig.hpp"

#include <algorithm>
#include <cfloat>
#include <set>
#include <stdexcept>
#include <unordered_set>

#include <boost/algorithm/string/case_conv.hpp>
#include <boost/algorithm/string/replace.hpp>
#include <boost/format.hpp>
#include <boost/lexical_cast.hpp>
#include <boost/log/trivial.hpp>
#include <boost/nowide/iostream.hpp>
#include <boost/thread.hpp>

#include "ConfigDef.hpp"
#include "Flow.hpp"
#include "format.hpp"
#include "GCode/Thumbnails.hpp"
#include "I18N.hpp"
#include "PointUtils.hpp"
#include "PrintSteps.hpp"
#include "Semver.hpp"
#include "SLA/SupportTree.hpp"
#include "Utils.hpp"

namespace Slic3r {

static t_config_enum_names enum_names_from_keys_map(const t_config_enum_values &enum_keys_map)
{
    t_config_enum_names names;
    int cnt = 0;
    for (const auto& kvp : enum_keys_map)
        cnt = std::max(cnt, kvp.second);
    cnt += 1;
    names.assign(cnt, "");
    for (const auto& kvp : enum_keys_map)
        names[kvp.second] = kvp.first;
    return names;
}

#define CONFIG_OPTION_ENUM_DEFINE_STATIC_MAPS(NAME) \
    static t_config_enum_names s_keys_names_##NAME = enum_names_from_keys_map(s_keys_map_##NAME); \
    template<> const t_config_enum_values& ConfigOptionEnum<NAME>::get_enum_values() { return s_keys_map_##NAME; } \
    template<> const t_config_enum_names& ConfigOptionEnum<NAME>::get_enum_names() { return s_keys_names_##NAME; }


static const t_config_enum_values s_keys_map_PrinterTechnology{
    {"FFF",             ptFFF },
    {"SLA",             ptSLA },
    {"SLS",             ptSLS},
    {"CNC",             ptMill},
    {"LSR",             ptLaser},
};
CONFIG_OPTION_ENUM_DEFINE_STATIC_MAPS(PrinterTechnology)


static const t_config_enum_values s_keys_map_OutputFormat {
    {"SL1", ofSL1},
    {"SL1_SVG", ofSL1_SVG},
    {"mCWS", ofMaskedCWS},
    {"AnyMono", ofAnycubicMono},
    {"AnyMonoX", ofAnycubicMonoX},
    {"AnyMonoSE", ofAnycubicMonoSE},
};
CONFIG_OPTION_ENUM_DEFINE_STATIC_MAPS(OutputFormat)


static const t_config_enum_values s_keys_map_PrintHostType {
    { "prusalink",      htPrusaLink },
    { "prusaconnect",   htPrusaConnect },
    { "octoprint",      htOctoPrint },
    { "moonraker",      htMoonraker },
    { "duet",           htDuet },
    { "flashair",       htFlashAir },
    { "astrobox",       htAstroBox },
    { "repetier",       htRepetier },
    {"klipper",         htKlipper},
    {"mpmdv2",          htMPMDv2},
    { "mks",            htMKS },
    {"monoprice",       htMiniDeltaLCD },
};
CONFIG_OPTION_ENUM_DEFINE_STATIC_MAPS(PrintHostType)

static const t_config_enum_values s_keys_map_AuthorizationType {
    {"key", atKeyPassword},
    {"user", atUserPassword},
};
CONFIG_OPTION_ENUM_DEFINE_STATIC_MAPS(AuthorizationType)


static const t_config_enum_values s_keys_map_SlicingMode {
    { "regular",        int(SlicingMode::Regular) },
    { "even_odd",       int(SlicingMode::EvenOdd) },
    { "close_holes",    int(SlicingMode::CloseHoles) }
};
CONFIG_OPTION_ENUM_DEFINE_STATIC_MAPS(SlicingMode)


//unused
//static const t_config_enum_values s_keys_map_SupportMaterialInterfacePattern {
//    { "auto",           smipAuto },
//    { "rectilinear",    smipRectilinear },
//    { "concentric",     smipConcentric }
//};
//CONFIG_OPTION_ENUM_DEFINE_STATIC_MAPS(SupportMaterialInterfacePattern)


// Orca


// unused


static const t_config_enum_values s_keys_map_ZLiftTop{
    {"everywhere", zltAll},
    {"onlytop", zltTop},
    {"nottop", zltNotTop},
};
CONFIG_OPTION_ENUM_DEFINE_STATIC_MAPS(ZLiftTop);

static const t_config_enum_values s_keys_map_ForwardCompatibilitySubstitutionRule{
    { "disable",        ForwardCompatibilitySubstitutionRule::Disable },
    { "enable",         ForwardCompatibilitySubstitutionRule::Enable },
    { "enable_silent",  ForwardCompatibilitySubstitutionRule::EnableSilent },
};
CONFIG_OPTION_ENUM_DEFINE_STATIC_MAPS(ForwardCompatibilitySubstitutionRule);


static void assign_printer_technology_to_unknown(t_optiondef_map &options, PrinterTechnology printer_technology)
{
    for (std::pair<const t_config_option_key, ConfigOptionDef> &kvp : options)
        if (kvp.second.printer_technology == ptUnknown)
            kvp.second.printer_technology = printer_technology;
}

// Maximum extruder temperature, bumped to 1500 to support printing of glass.
namespace {
    const int max_temp = 1500;
};

ConfigOption *disable_default_option(ConfigOption *option) {
    return option->set_can_be_disabled(true);
}

ConfigOption *enable_default_option(ConfigOption *option) {
    return option->set_can_be_disabled(false);
}

ConfigOptionVectorBase *disable_default_option(ConfigOptionVectorBase *option, bool default_is_disabled = true) {
    return (ConfigOptionVectorBase *)option->set_can_be_disabled(true);
}

ConfigOptionVectorBase *enable_default_option(ConfigOptionVectorBase *option, bool default_is_disabled = true) {
    return (ConfigOptionVectorBase *)option->set_can_be_disabled(false);
}

PrintConfigDef::PrintConfigDef() : ConfigDef() {
    m_key_categories.resize(RAW_PRESET_TYPE_COUNT);
}

const std::set<std::string> &PrintConfigDef::extruder_option_keys() const {
    return option_keys(RAW_PRESET_TYPE_FFF_TOOL_EXTRUDER);
}

const std::set<std::string> &PrintConfigDef::filament_override_option_keys() const {
    return option_keys(RAW_PRESET_TYPE_FFF_FILAMENT_OVERRIDE);
}

const std::set<std::string> &PrintConfigDef::extruder_retract_keys() const {
    return option_keys(RAW_PRESET_TYPE_FFF_TOOL_EXTRUDER_RETRACTION);
}

const std::set<std::string> &PrintConfigDef::milling_option_keys() const {
    return option_keys(RAW_PRESET_TYPE_FFF_TOOL_MILLING);
}

const std::set<std::string> &PrintConfigDef::material_overrides_option_keys() const {
     return option_keys(RAW_PRESET_TYPE_SLA_MATERIAL);
}

const std::set<std::string> &PrintConfigDef::option_keys(raw_option_preset_type type) const {
    return m_key_categories[type];
}

std::set<std::string> &PrintConfigDef::option_keys(raw_option_preset_type type) {
    assert(!is_finalized());
    return m_key_categories[type];
}

void PrintConfigDef::assign_printer_technology_to_unknown(PrinterTechnology printer_technology)
{
    Slic3r::assign_printer_technology_to_unknown(this->options, printer_technology);
}

void PrintConfigDef::init_common_params()
{
    assert(!is_finalized());

    ConfigOptionDef* def;

    // version of the settings:
    // 4 letters for the software (SUSI, PRSA, BMBU, ORCA)
    // a '_'
    // version in X.X.X.X
    def = this->add("print_version", coString);
    def->printer_technology = ptFFF | ptSLA;
    // defautl to none : only set if loaded. only write our version
    def->set_default_value(new ConfigOptionStringVersion());
    def->cli = ConfigOptionDef::nocli;
    def->can_phony = true;

    def = this->add("printer_technology", coEnum);
    def->label = L("Printer technology");
    def->tooltip = L("Printer technology");
    def->printer_technology = ptFFF | ptSLA;
    def->category = OptionCategory::general;
    def->set_enum<PrinterTechnology>({ "FFF", "SLA" });
    def->mode = comSimpleAE | comPrusa;
    def->set_default_value(new ConfigOptionEnum<PrinterTechnology>(ptFFF));

    def = this->add("bed_shape", coPoints);
    def->label = L("Bed shape");
    def->category = OptionCategory::general;
    def->invalidates_step = psGCodeExport;
    def->mode = comAdvancedE | comPrusa;
    def->set_default_value(new ConfigOptionPoints{ Vec2d(0, 0), Vec2d(200, 0), Vec2d(200, 200), Vec2d(0, 200) });

    def = this->add("bed_custom_texture", coString);
    def->label = L("Bed custom texture");
    def->printer_technology = ptFFF | ptSLA;
    def->category = OptionCategory::general;
    def->mode = comAdvancedE | comPrusa;
    def->set_default_value(new ConfigOptionString(""));

    def = this->add("bed_custom_model", coString);
    def->label = L("Bed custom model");
    def->printer_technology = ptFFF | ptSLA;
    def->category = OptionCategory::general;
    def->mode = comAdvancedE | comPrusa;
    def->set_default_value(new ConfigOptionString(""));

    def = this->add("thumbnails", coPoints);
    def->label = L("Thumbnails size");
    def->tooltip = L("Picture sizes to be stored into a .gcode / .bgcode and .sl1 / .sl1s files, in the following format: \"XxY/EXT, XxY/EXT, ...\"\n"
                     "Currently supported extensions are PNG, QOI and JPG.");
    def->printer_technology = ptFFF | ptSLA;
    def->category = OptionCategory::firmware;
    def->invalidates_step = psGCodeExport;
    def->mode = comExpert | comPrusa;
    def->min = 0;
    def->max = 2048;
    def->set_default_value(new ConfigOptionPoints{ std::initializer_list<Vec2d>{ Vec2d(0,0), Vec2d(0,0) } });

    def = this->add("thumbnails_color", coString);
    def->label = L("Color");
    def->full_label = L("Thumbnail color");
    def->printer_technology = ptFFF | ptSLA;
    def->category = OptionCategory::filament;
    def->invalidates_step = psGCodeExport;
    def->tooltip = L("This is the color that will be enforced on objects in the thumbnails.");
    def->gui_type = ConfigOptionDef::GUIType::color;
    def->mode = comExpert | comSuSi;
    def->set_default_value(new ConfigOptionString("#018aff"));

    def = this->add("thumbnails_custom_color", coBool);
    def->label = L("Enforce thumbnail color");
    def->tooltip = L("Enforce a specific color on thumbnails."
        " If not enforced, their color will be the one defined by the filament.");
    def->printer_technology = ptFFF | ptSLA;
    def->category = OptionCategory::firmware;
    def->invalidates_step = psGCodeExport;
    def->mode = comExpert | comSuSi;
    def->set_default_value(new ConfigOptionBool(false));

    def = this->add("thumbnails_end_file", coBool);
    def->label = L("Print at the end");
    def->invalidates_step = psGCodeExport;
    def->tooltip = L("Print the thumbnail code at the end of the gcode file instead of the front."
        "\nBe careful! Most firmwares expect it at the front, so be sure that your firmware support it.");
    def->mode = comExpert | comSuSi;
    def->set_default_value(new ConfigOptionBool(false)); 

    def = this->add("thumbnails_with_bed", coBool);
    def->label = L("Bed on thumbnail");
    def->tooltip = L("Show the bed texture on the thumbnail picture.");
    def->printer_technology = ptFFF | ptSLA;
    def->category = OptionCategory::firmware;
    def->invalidates_step = psGCodeExport;
    def->mode = comExpert | comSuSi;
    def->set_default_value(new ConfigOptionBool(true));

    def = this->add("thumbnails_format", coEnum);
    def->label = L("Format of G-code thumbnails");
    def->tooltip = L("Format of G-code thumbnails: PNG for best quality, JPG for smallest size, QOI for low memory firmware");
    def->printer_technology = ptFFF | ptSLA;
    def->category = OptionCategory::firmware;
    def->invalidates_step = psGCodeExport;
    def->mode = comExpert | comPrusa;
    def->set_enum<GCodeThumbnailsFormat>({ "PNG", "JPG", "QOI", "BIQU" });
    def->set_default_value(new ConfigOptionEnum<GCodeThumbnailsFormat>(GCodeThumbnailsFormat::PNG));

    def          = this->add("thumbnails_tag_format", coBool);
    def->label   = L("Write the thumbnail type in gcode.");
    def->tooltip = L("instead of writing 'thumbnails' as tag in the gcode, it will write 'thumbnails_PNG', thumbnails_JPG', 'thumbnail_QOI', etc.."
        "\n Some firmware need it to know how to decode the thumbnail, some others don't support it.");
    def->printer_technology = ptFFF | ptSLA;
    def->category = OptionCategory::firmware;
    def->invalidates_step = psGCodeExport;
    def->mode    = comExpert | comSuSi;
    def->set_default_value(new ConfigOptionBool(false));

    def = this->add("thumbnails_with_support", coBool);
    def->label = L("Support on thumbnail");
    def->tooltip = L("Show the supports (and pads) on the thumbnail picture.");
    def->printer_technology = ptFFF | ptSLA;
    def->category = OptionCategory::firmware;
    def->mode = comExpert | comSuSi;
    def->set_default_value(new ConfigOptionBool(false));

    def = this->add("layer_height", coFloat);
    def->label = L("Base Layer height");
    def->category = OptionCategory::slicing;
    def->tooltip = L("This setting controls the height (and thus the total number) of the slices/layers. "
        "Thinner layers give better accuracy but take more time to print.");
    def->printer_technology = ptFFF | ptSLA;
    def->category = OptionCategory::slicing;
    def->invalidates_step = posSlice;
    def->sidetext = L("mm");
    def->min = 0;
    def->mode = comAdvancedE | comPrusa;
    def->set_default_value(new ConfigOptionFloat(0.2));

    def = this->add("max_print_height", coFloat);
    def->label = L("Max print height");
    def->printer_technology = ptFFF | ptSLA;
    def->category = OptionCategory::general;
    def->invalidates_step = psGCodeExport;
    def->tooltip = L("Set this to the maximum height that can be reached by your extruder while printing.");
    def->sidetext = L("mm");
    def->min = 0;
    def->mode = comAdvancedE | comPrusa;
    def->set_default_value(new ConfigOptionFloat(200.0));

    def = this->add("print_host", coString);
    def->label = L("Hostname, IP or URL");
    def->category = OptionCategory::general;
    def->tooltip = L("Slic3r can upload G-code files to a printer host. This field should contain "
                   "the hostname, IP address or URL of the printer host instance. "
                   "Print host behind HAProxy with basic auth enabled can be accessed by putting the user name and password into the URL "
                   "in the following format: https://username:password@your-octopi-address/");
    def->mode = comAdvancedE | comPrusa;
    def->cli = ConfigOptionDef::nocli;
    def->set_default_value(new ConfigOptionString(""));

    def = this->add("printhost_apikey", coString);
    def->label = L("API Key / Password");
    def->printer_technology = ptFFF | ptSLA;
    def->category = OptionCategory::general;
    def->tooltip = L("Slic3r can upload G-code files to a printer host. This field should contain "
                   "the API Key or the password required for authentication.");
    def->mode = comAdvancedE | comPrusa;
    def->cli = ConfigOptionDef::nocli;
    def->set_default_value(new ConfigOptionString(""));
    
    // for repetier
    def = this->add("printhost_port", coString);
    def->label = L("Printer");
    def->tooltip = L("Name of the printer");
    def->printer_technology = ptFFF | ptSLA;
    def->mode = comAdvancedE | comPrusa;
    def->cli = ConfigOptionDef::nocli;
    def->set_enum_values(ConfigOptionDef::GUIType::select_open, {"no printers"});
    def->set_default_value(new ConfigOptionString(""));
    
    // only if there isn't a native SSL support
    def = this->add("printhost_cafile", coString);
    def->label = L("HTTPS CA File");
    def->printer_technology = ptFFF | ptSLA;
    def->category = OptionCategory::general;
    def->tooltip = L("Custom CA certificate file can be specified for HTTPS OctoPrint connections, in crt/pem format. "
                   "If left blank, the default OS CA certificate repository is used.");
    def->mode = comAdvancedE | comPrusa;
    def->cli = ConfigOptionDef::nocli;
    def->set_default_value(new ConfigOptionString(""));

    def = this->add("printhost_client_cert", coString);
    def->label = L("Client Certificate File");
    def->printer_technology = ptFFF | ptSLA;
    def->category = OptionCategory::general;
    def->tooltip = L("A Client certificate file for use with 2-way ssl authentication, in p12/pfx format. "
                   "If left blank, no client certificate is used.");
    def->mode = comAdvancedE | comSuSi;
    def->set_default_value(new ConfigOptionString(""));

    def = this->add("printhost_client_cert_password", coString);
    def->label = L("Client Certificate Password");
    def->category = OptionCategory::general;
    def->tooltip = L("Password for client certificate for 2-way ssl authentication. "
                   "Leave blank if no password is needed");
    def->mode = comAdvancedE | comSuSi;
    def->set_default_value(new ConfigOptionString(""));

    // For PrusaLink
    def = this->add("printhost_user", coString);
    def->label = L("User");
//    def->tooltip = L("");
    def->printer_technology = ptFFF | ptSLA;
    def->mode = comAdvancedE | comPrusa;
    def->cli = ConfigOptionDef::nocli;
    def->set_default_value(new ConfigOptionString(""));

    // For PrusaLink
    def = this->add("printhost_password", coString);
    def->label = L("Password");
//    def->tooltip = L("");
    def->printer_technology = ptFFF | ptSLA;
    def->mode = comAdvancedE | comPrusa;
    def->cli = ConfigOptionDef::nocli;
    def->set_default_value(new ConfigOptionString(""));

    // Only available on Windows.
    def = this->add("printhost_ssl_ignore_revoke", coBool);
    def->label = L("Ignore HTTPS certificate revocation checks");
    def->tooltip = L("Ignore HTTPS certificate revocation checks in case of missing or offline distribution points. "
        "One may want to enable this option for self signed certificates if connection fails.");
    def->printer_technology = ptFFF | ptSLA;
    def->mode = comAdvancedE | comPrusa;
    def->cli = ConfigOptionDef::nocli;
    def->set_default_value(new ConfigOptionBool(false));
    
    def = this->add("preset_names", coStrings);
    def->label = L("Printer preset names");
    def->tooltip = L("Names of presets related to the physical printer");
    def->mode = comAdvancedE | comPrusa;
    def->set_default_value(new ConfigOptionStrings{});

    // For PrusaLink
    def = this->add("printhost_authorization_type", coEnum);
    def->label = L("Authorization Type");
//    def->tooltip = L("");
    def->printer_technology = ptFFF | ptSLA;
    def->set_enum<AuthorizationType>({
        { "key", L("API key") },
        { "user", L("HTTP digest") }
    });
    def->mode = comAdvancedE | comPrusa;
    def->cli = ConfigOptionDef::nocli;
    def->set_default_value(new ConfigOptionEnum<AuthorizationType>(atKeyPassword));

    // temporary workaround for compatibility with older Slicer
    {
        def = this->add("preset_name", coString);
        def->printer_technology = ptFFF | ptSLA;
        def->set_default_value(new ConfigOptionString());
    }
}

// Ignore the following obsolete configuration keys:
static std::set<t_config_option_key> PrintConfigDef_ignore = {
    "clip_multipart_objects",
    "duplicate_x", "duplicate_y", "gcode_arcs", "multiply_x", "multiply_y",
    "support_material_tool", "acceleration", "adjust_overhang_flow",
    "standby_temperature", "scale", "rotate", "duplicate", "duplicate_grid",
    "start_perimeters_at_concave_points", "start_perimeters_at_non_overhang", "randomize_start",
    "seal_position", "vibration_limit", "bed_size",
    "print_center", "g0", "threads", "pressure_advance", "wipe_tower_per_color_wipe",
    "cooling", "serial_port", "serial_speed",
    "exact_last_layer_height",
    "threads",
    // Introduced in some PrusaSlicer 2.3.1 alpha, later renamed or removed.
    "fuzzy_skin_perimeter_mode", "fuzzy_skin_shape",
    //replaced by brim_per_object, but can't translate the value as the old one is only used for complete_objects (and the default are differents).
    "complete_objects_one_brim",
    // Introduced in PrusaSlicer 2.3.0-alpha2, later replaced by automatic calculation based on extrusion width.
    "wall_add_middle_threshold", "wall_split_middle_threshold",
    // Replaced by new concentric ensuring in 2.6.0-alpha5
//    "infill_only_where_needed", <- ignore only if deactivated
    "gcode_binary", // Introduced in 2.7.0-alpha1, removed in 2.7.1 (replaced by binary_gcode).
    "gcode_resolution", // now in printer config.
    "enable_dynamic_fan_speeds", "overhang_fan_speed_0","overhang_fan_speed_1","overhang_fan_speed_2","overhang_fan_speed_3", // converted in composite_legacy
    "enable_dynamic_overhang_speeds", "overhang_speed_0", "overhang_speed_1", "overhang_speed_2", "overhang_speed_3", // converted in composite_legacy
    "travel_max_lift", "filament_travel_max_lift", // removed, using retract_lift also for rampping lift instead.
    "small_area_infill_flow_compensation",
};
namespace Handle_legacy_tools {

std::unordered_map<t_config_option_key, std::pair<t_config_option_key, std::string>>::iterator last_search_result;

typedef std::pair<t_config_option_key, std::string> KVEntry;
inline void for_ech_entry(std::unordered_map<t_config_option_key, std::pair<t_config_option_key, std::string>> &dict,
                          std::initializer_list<t_config_option_key> &&list,
                          const std::function<void(t_config_option_key &opt_key, std::string &value)> &do_something) {
    for (const t_config_option_key &key : list) {
        if (auto last_search_result = dict.find(key); last_search_result != dict.end()) {
            // assert(last_search_result->second.first == key); it's possibly different because of alias.
            do_something(last_search_result->second.first, last_search_result->second.second);
        }
    }
}
inline void for_ech_entry(std::unordered_map<t_config_option_key, std::pair<t_config_option_key, std::string>> &dict,
                          const std::set<t_config_option_key> &list,
                          const std::function<void(t_config_option_key &opt_key, std::string &value)> &do_something) {
    for (const t_config_option_key &key : list) {
        if (last_search_result = dict.find(key); last_search_result != dict.end()) {
            do_something(last_search_result->second.first, last_search_result->second.second);
        }
    }
}
inline bool has(std::unordered_map<t_config_option_key, std::pair<t_config_option_key, std::string>> &dict,
                 const t_config_option_key &opt_key) {
    last_search_result = dict.find(opt_key);
    // exists and not already deleted/changed
    return last_search_result != dict.end() && last_search_result->second.first == opt_key;
}
inline bool has(std::unordered_map<t_config_option_key, std::pair<t_config_option_key, std::string>> &dict,
                  const t_config_option_key &&opt_key,
                  const std::string &&value) {
    last_search_result = dict.find(opt_key);
    return last_search_result != dict.end() && last_search_result->second.first == opt_key && last_search_result->second.second == value;
}
//inline KVEntry &get(std::unordered_map<t_config_option_key, std::pair<t_config_option_key, std::string>> &dict,
//                    const t_config_option_key &&opt_key) {
//    assert(dict.find(opt_key) != dict.end());
//    return dict.at(opt_key);
//}
//inline void set(std::unordered_map<t_config_option_key, std::pair<t_config_option_key, std::string>> &dict,
//                const t_config_option_key &&opt_key,
//                t_config_option_key &&new_key,
//                std::string &&new_val) {
//    assert(dict.find(opt_key) != dict.end());
//    KVEntry &entry = dict.at(opt_key);
//    entry.first = std::move(new_key);
//    entry.second = std::move(new_val);
//}
//inline void set(std::unordered_map<t_config_option_key, std::pair<t_config_option_key, std::string>> &dict,
//                const t_config_option_key &&opt_key,
//                t_config_option_key &&new_key) {
//    assert(dict.find(opt_key) != dict.end());
//    KVEntry &entry = dict.at(opt_key);
//    entry.first = std::move(new_key);
//}

inline void set(t_config_option_key &&new_key,
                std::string &&new_val) {
    last_search_result->second.first = std::move(new_key);
    last_search_result->second.second = std::move(new_val);
}

inline std::string &value() {
    return last_search_result->second.second;
}

//inline std::string &value(
//    std::unordered_map<t_config_option_key, std::pair<t_config_option_key, std::string>>::iterator &it) {
//    return it->second.second;
//}
//
//inline std::string &value(
//    std::unordered_map<t_config_option_key, std::pair<t_config_option_key, std::string>> &dict,
//                    const t_config_option_key &&opt_key) {
//    assert(dict.find(opt_key) != dict.end());
//    return dict.at(opt_key).second;
//}

inline t_config_option_key &opt_key() {
    return last_search_result->second.first;
}

//inline t_config_option_key &opt_key(
//    std::unordered_map<t_config_option_key, std::pair<t_config_option_key, std::string>>::iterator &it) {
//    return it->second.first;
//}
//
//inline t_config_option_key &opt_key(
//    std::unordered_map<t_config_option_key, std::pair<t_config_option_key, std::string>> &dict,
//                    const t_config_option_key &&opt_key) {
//    assert(dict.find(opt_key) != dict.end());
//    return dict.at(opt_key).first;
//}

//inline void erase(std::unordered_map<t_config_option_key, std::pair<t_config_option_key, std::string>>::iterator &it) {
//    it->second.first = "";
//    it->second.second = "";
//}

const std::vector<std::pair<t_config_option_key, t_config_option_key>> widths_2_spacings_for_phony_fix =
    {{"extrusion_width", "extrusion_spacing"},
     {"perimeter_extrusion_width", "perimeter_extrusion_spacing"},
     {"external_perimeter_extrusion_width", "external_perimeter_extrusion_spacing"},
     {"first_layer_extrusion_width", "first_layer_extrusion_spacing"},
     {"infill_extrusion_width", "infill_extrusion_spacing"},
     {"solid_infill_extrusion_width", "solid_infill_extrusion_spacing"},
     {"top_infill_extrusion_width", "top_infill_extrusion_spacing"}};

inline void erase() {
    last_search_result->second.first = "";
}

void _handle_legacy(std::unordered_map<t_config_option_key, std::pair<t_config_option_key, std::string>> &dict, bool remove_unkown_keys)
{
    using namespace std::literals;
    typedef t_config_option_key Key;
    typedef std::string Val;
    // handle legacy options (other than aliases)
    for_ech_entry(dict, {"extrusion_width_ratio", "bottom_layer_speed_ratio", "first_layer_height_ratio"},
                  [](Key &opt_key, Val &value) {
                boost::replace_first(opt_key, "_ratio", "");
                if (opt_key == "bottom_layer_speed")
                    opt_key = "first_layer_speed";
                try {
                    float v = boost::lexical_cast<float>(value);
                    if (v != 0)
                        value = to_string_nozero(v * 100, 2) + "%";
                } catch (boost::bad_lexical_cast &) { value = "0"; }
            });
    if (has(dict, "infill_only_where_needed"s, "0"s)) {
        erase();
    }
    if (has(dict, "gcode_flavor")) {
        if (value() == "makerbot")
            value() = "makerware";
        else if (value() == "marlinfirmware")
            // the "new" marlin firmware flavor used to be called "marlinfirmware" for some time during PrusaSlicer 2.4.0-alpha development.
            value() = "marlin2";
    }
    if (has(dict, "gcode_firmware_plugin", "gcode.firmware.default")) {
        // The former generic provider was the Marlin 2 implementation. Keep
        // existing printer presets on the equivalent explicit dialect.
        value() = "gcode.firmware.marlin2";
    }
    if (has(dict, "host_type"s, "mainsail"s)) {
        // the "mainsail" key (introduced in 2.6.0-alpha6) was renamed to "moonraker" (in 2.6.0-rc1).
        set("host_type", "moonraker");
    }
    if (has(dict, "fill_density") && value().find("%") == std::string::npos) {
        try {
            // fill_density was turned into a percent value
            float v = boost::lexical_cast<float>(value());
            value() = to_string_nozero(v * 100, 2) + "%";
        } catch (boost::bad_lexical_cast &) { erase(); }
    }
    if (has(dict, "randomize_start", "1")) {
        set("seam_position", "random");
    }
    if (has(dict, "bed_size") && !value().empty()) {
        opt_key() = "bed_shape";
        ConfigOptionPoint p;
        p.deserialize(value(), ForwardCompatibilitySubstitutionRule::Disable);
        std::ostringstream oss;
        oss << "0x0," << p.value(0) << "x0," << p.value(0) << "x" << p.value(1) << ",0x" << p.value(1);
        value() = oss.str();
    }
    //if ((opt_key == "perimeter_acceleration" && value == "25")
    //    || (opt_key == "infill_acceleration" && value == "50")) {
    //    /*  For historical reasons, the world's full of configs having these very low values;
    //        to avoid unexpected behavior we need to ignore them. Banning these two hard-coded
    //        values is a dirty hack and will need to be removed sometime in the future, but it
    //        will avoid lots of complaints for now. */
    //    value = "0";
    //} // i think it's time now.
    //TODO: change defautl from 0 (no forbidden) to 100% for all speed & acceleration
    //if ("0" == value && ("infill_acceleration" == opt_key || "solid_infill_acceleration" == opt_key ||
    //    "top_solid_infill_acceleration" == opt_key || "bridge_acceleration" == opt_key ||
    //    "default_acceleration" == opt_key || "perimeter_acceleration" == opt_key || "overhangs_speed" == opt_key ||
    //    "ironing_speed" == opt_key || "perimeter_speed" == opt_key || "infill_speed" == opt_key ||
    //    "bridge_speed" == opt_key || "support_material_speed" == opt_key || "max_print_speed" == opt_key)) {
    //    // 100% is the same, and easier to understand.
    //    value = "100%";
    //}
    if (has(dict, "support_material_pattern", "pillars")) {
        // Slic3r PE does not support the pillars. They never worked well.
        value() = "rectilinear";
    }
    if (has(dict, "skirt_height"s, "-1"s)) {
        // PrusaSlicer no more accepts skirt_height == -1 to print a draft shield to the top of the highest object.
        // A new "draft_shield" enum config value is used instead.
        set("draft_shield"s, "enabled"s);
    }
    if (has(dict, "draft_shield") && (value() == "1" || value() == "0")) {
        // draft_shield used to be a bool, it was turned into an enum in PrusaSlicer 2.4.0.
        value() = value() == "1" ? "enabled" : "disabled";
    }
    for_ech_entry(dict, {"label_printed_objects", "gcode_label_objects"},
                  [](Key &opt_key, Val &value) {
        // gcode_label_objects used to be a bool (the behavior was nothing or "octoprint"), it is
        // and enum since PrusaSlicer 2.6.2.
        if ((value == "1" || value == "0"))
            value = value == "1" ? "octoprint" : "disabled";
    });
    if (has(dict, "octoprint_host"s)) {
        opt_key() = "print_host"s;
    }
    if (has(dict, "octoprint_cafile"s)) {
        opt_key() = "printhost_cafile"s;
    }
    if (has(dict, "octoprint_apikey"s)) {
        opt_key() = "printhost_apikey"s;
    }
    if (has(dict, "elefant_foot_compensation"s)) {
        float v = boost::lexical_cast<float>(value());
        opt_key() = "first_layer_size_compensation";
        if (v > 0)
            value() = to_string_nozero(-v, 5);
    }
    if (has(dict, "elefant_foot_min_width"s)) {
        opt_key() = "elephant_foot_min_width"s;
    }
    if (has(dict, "thumbnails"s)) {
        if (value().empty())
            value() = "0x0,0x0";
    }
    if (has(dict, "z_steps_per_mm"s)) {
        float v = boost::lexical_cast<float>(value());
        opt_key() = "z_step";
        if (v > 0)
            value() = to_string_nozero(1/v, 5);
    }
    if (has(dict, "infill_not_connected"s) ) {
        opt_key() = "infill_connection";
        if (value() == "1")
            value() = "notconnected";
        else
            value() = "connected";
    }
    if (has(dict, "preset_name"s)) {
        opt_key() = "preset_names"s;
    }
    if (has(dict, "ensure_vertical_shell_thickness"s)) {
        if (value() == "1") {
            value() = "enabled_old";
        } else if (value() == "0") {
            value() = "disabled";
        } else if (const t_config_enum_values &enum_keys_map = ConfigOptionEnum<EnsureVerticalShellThickness>::get_enum_values(); enum_keys_map.find(value()) == enum_keys_map.end()) {
            assert(value() == "0" || value() == "1");
            // Values other than 0/1 are replaced with "partial" for handling values from different slicers.
            value() = "partial";
        }
    }
    if (has(dict, "seam_travel"s)) {
        if (value() == "1") {
            opt_key() = "seam_travel_cost";
            value() = "200%";
        } else {
            erase();
        }
    }
    if (has(dict, "seam_position"s)) {
        if (value() == "hidden") {
            value() = "cost";
        } else if ("near" == value() || "nearest" == value()) {
            value() = "cost";
            // we change the cost
            //note: modifying dict invalidate opt_key() & value()
            dict["seam_angle_cost"] = {"seam_angle_cost", "50%"};
            dict["seam_travel_cost"] = {"seam_travel_cost", "50%"};
        }
    }
    if (has(dict, "perimeter_loop_seam"s)) {
        if (value() == "hidden") {
            value() = "nearest";
        }
    }
    //if (has(dict, "overhangs"s)) {
    //    opt_key() = "overhangs_width_speed";
    //    if (value() == "1") {
    //        value() = "50%";
    //        //note: modifying dict invalidate opt_key() & value()
    //        dict["overhangs_width"] = {"overhangs_width", "50%"};
    //    } else {
    //        value() = "!50%";
    //        dict["overhangs_width"] = {"overhangs_width", "!50%"};
    //    }
    //}
    if (has(dict, "print_machine_envelope"s)) {
        opt_key() = "machine_limits_usage";
        if (value() == "1")
            value() = "emit_to_gcode";
        else
            value() = "time_estimate_only";
    }
    if (has(dict, "retract_lift_not_last_layer"s)) {
        opt_key() = "retract_lift_top";
        if (value() == "1")
            value() = "Not on top";
        else
            value() = "All surfaces";
    }
    if (has(dict, "gcode_precision_e"s)) {
        if (value().find(",") != std::string::npos)
            value() = value().substr(0, value().find(","));
        try {
            int val = boost::lexical_cast<int>(value());
            if (val > 0)
                value() = to_string_nozero(val, 5);
        }
        catch (boost::bad_lexical_cast&) {
            value() = "5";
        }
    }
    if (has(dict, "first_layer_min_speed") && !value().empty() && value().back() == '%')
        value() = value().substr(0, value().length() - 1); //no percent.
    
    for_ech_entry(dict, {"bridge_flow_ratio", "bridge_flow_ratio", "over_bridge_flow_ratio", "fill_top_flow_ratio", "first_layer_flow_ratio"},
                  [](Key &opt_key, Val &value) {
        // gcode_label_objects used to be a bool (the behavior was nothing or "octoprint"), it is
        // and enum since PrusaSlicer 2.6.2.
        if (!value.empty() && value.back() != '%') {
            // need percent
            try {
                float val = boost::lexical_cast<float>(value);
                if (val < 2)
                    value = to_string_nozero(val * 100, 2) + "%";
                else
                    value = "100%";
            } catch (boost::bad_lexical_cast &) { value = "100%"; }
        }
    });
    //if (has(dict, "thick_bridges"s)) {
    //    assert(dict.find("bridge_type") == dict.end());
    //    assert(dict.find("bridge_overlap") == dict.end());
    //    assert(dict.find("bridge_overlap_min") == dict.end());
    //    opt_key() = "bridge_type"s;
    //    if (value() == "1") {
    //        value() = "nozzle";
    //        dict["bridge_overlap_min"] = {"bridge_overlap_min", "80%"};
    //        dict["bridge_overlap"] = {"bridge_overlap", "95%"};
    //    } else {
    //        value() = "flow";
    //        dict["bridge_overlap_min"] = {"bridge_overlap_min", "60%"};
    //        dict["bridge_overlap"] = {"bridge_overlap", "75%"};
    //    }
    //}
    //if (has(dict, "sla_archive_format"s)) {
    //    opt_key() = "output_format"s;
    //}

    // In PrusaSlicer 2.3.0-alpha0 the "monotonic" infill was introduced, which was later renamed to "monotonous".
    for_ech_entry(dict,
                  {"top_fill_pattern", "bottom_fill_pattern", "fill_pattern", "solid_fill_pattern",
                   "bridge_fill_pattern", "support_material_interface_pattern",
                   "support_material_top_interface_pattern", "support_material_bottom_interface_pattern"},
                  [&dict](Key &opt_key, Val &value) {
        // gcode_label_objects used to be a bool (the behavior was nothing or "octoprint"), it is
        // and enum since PrusaSlicer 2.6.2.
        if (value == "monotonous") {
            value = "monotonic";
        }
        bool set_gapfill =false;
        if (value == "rectilineargapfill") {
            value = "rectilinear";
            set_gapfill = true;
        }
        if (value == "monotonicgapfill") {
            value="monotonic";
            set_gapfill = true;
        }
        if (value == "concentricgapfill") {
            value="concentric";
            set_gapfill = true;
        }
        if (set_gapfill) {
            if (opt_key == "bottom_fill_pattern") {
                //note: modifying dict invalidate opt_key & value
                dict["infill_filled_bottom"] = {"infill_filled_bottom", "1"};
            } else if (opt_key == "solid_fill_pattern") {
                dict["infill_filled_solid"] = {"infill_filled_solid", "1"};
            } else if (opt_key == "top_fill_pattern") {
                dict["infill_filled_top"] = {"infill_filled_top", "1"};
            }
        }
    });

    //in ps 2.4, the raft_first_layer_density is now more powerful than the support_material_solid_first_layer, also it always does the perimeter.
    if (has(dict, "support_material_solid_first_layer"s)) {
        opt_key() = "raft_first_layer_density"s;
        value() = "100";
    }
    // thin perimeters are now a threshold instead of a bool
    for_ech_entry(dict, {"thin_perimeters", "thin_perimeters_all"},
                  [](Key &opt_key, Val &value) {
        if (value == "1") {
            value = "100%";
        }
    });

    const std::vector<std::string> move_deactivate = {
        "overhangs_width"s, "overhangs_flow_ratio"s
        };
    for (int i = 0; i < move_deactivate.size(); i += 2) {
        // get our keyf
        if (has(dict, move_deactivate[i])) {
            // is it (now wrongly) deactivated?
            if (!value().empty() && value()[0] == '!') {
                value() = value().substr(1);
                // get our companion
                if (has(dict, move_deactivate[i + 1])) {
                    // deactivate it
                    if (value().empty()) {
                        value() = "!100";
                    } else if (value()[0] != '!') {
                        value() = std::string("!") + value();
                    }
                } else {
                    // or create it
                    dict[move_deactivate[i + 1]] = {move_deactivate[i + 1], "!100"};
                }
            }
        }
    }


    // prusa renamed "sprinter" "reprap"
    if (has(dict, "gcode_flavor"s)) {
        if ("reprap" == value())
            value() = "sprinter";
    }
    
    // remove ignored entries
    for_ech_entry(dict, PrintConfigDef_ignore,
                  [](Key &opt_key, Val &value) {
        erase();
    });

    if (has(dict, "fan_always_on"s)) {
        if (value() != "1") {
            //min_fan_speed is already converted to default_fan_speed, just has to deactivate it if not always_on
            opt_key() = "default_fan_speed"s; // note: maybe this doesn't works, as default_fan_speed can also get its value() from min_fan_speed
            value() = "0";
        } else {
            erase();
        }
    }
    if (has(dict, "arc_fitting"s)) {
        if (value() == "1")
            value() = "bambu";
        else if (value() == "0")
            value() = "disabled";
    }

    if (has(dict, "external_perimeters_vase")) {
        opt_key() = "seam_slope_type"s;
        if (value() == "1") {
            value() = "all";
        } else {
            value() = "none";
        }
    }

    // it's not needed to check aliases, because they are taken care of in deserialize().
    // still need as some things check for def and emit a ConfigSubstitutionContext
    for (auto it = dict.begin(); it != dict.end(); ++it) {
        if (!it->second.first.empty() && !PrintConfigDef::instance().has(it->second.first)) {
            // check the aliases
            for (const auto &entry : PrintConfigDef::instance().options) {
                for (const std::string &alias : entry.second.aliases) {
                    if (alias == it->second.first) {
                        // translate
                        it->second.first = entry.first;
                        goto use_alias;
                    }
                }
            }
            if (remove_unkown_keys) {
                it->second.first = "";
            }
        use_alias:;
        }
    }

    //fan speed: activate disable.
    assert(!has(dict, "bridge_internal_fan_speed"s));
    for_ech_entry(dict, {
        "bridge_fan_speed"s, "default_fan_speed"s, "min_fan_speed"s/* this is default_fan_speed's alias*/, "external_perimeter_fan_speed"s,
        "gap_fill_fan_speed"s, "infill_fan_speed"s, "internal_bridge_fan_speed"s, "bridge_internal_fan_speed"s, "overhangs_fan_speed"s,
        "perimeter_fan_speed"s, "solid_infill_fan_speed"s, "support_material_fan_speed"s, "support_material_interface_fan_speed"s, "top_fan_speed"s},
                  [](Key &opt_key, Val &value) {
            assert(PrintConfigDef::instance().get(opt_key) && PrintConfigDef::instance().get(opt_key)->type == coInts);
            //if vector, split it.
            ConfigOptionInts opt_decoder;
            opt_decoder.set_can_be_disabled();
            opt_decoder.deserialize(value);
            for (size_t idx = 0; idx < opt_decoder.size(); ++idx) {
                if (opt_decoder.is_enabled(idx)) {
                    if (opt_decoder.get_at(idx) < 0) {
                        opt_decoder.set_at(100, idx);
                        opt_decoder.set_enabled(false, idx);
                    } else if (opt_decoder.get_at(idx) <= 1) {
                        // for now, still consider "1" as a "0", to be able to import old config where the 1 means 0
                        // (and 0 was disable).
                        opt_decoder.set_at(0, idx);
                    }
                }
            }
            value = opt_decoder.serialize();
    });
    for(auto it = dict.begin(); it != dict.end(); ++it){
        std::string &opt_key = it->second.first;
        if (opt_key.empty()) {
            continue;
        }
        std::string &value = it->second.second;
        //array?
        if ("max_layer_height" == opt_key) {
            bool changed = false;
            std::vector<std::string> value_array;
            boost::split(value_array, value, boost::is_any_of(","), boost::token_compress_off);
            for (std::string &val : value_array) {
                if ("0" == val) {
                    val = "!75%";
                    changed = true;
                }
            }
            if (changed) {
                value = "";
                for (std::string &val : value_array) {
                    if (!value.empty()) {
                        value += ",";
                    }
                    value += val;
                }
            }
        }
        // 0-> disabled
        if ("0" == value) {
            if ("max_gcode_per_second" == opt_key) {
                value = "!0";
            }
            if ("gcode_min_length" == opt_key) {
                value = "!0";
            }
            if ("print_temperature" == opt_key) {
                value = "!0";
            }
            if ("print_first_layer_temperature" == opt_key) {
                value = "!0";
            }
        }
        //-1-> disabled
        if (value == "-1") {
            if (opt_key == "overhangs_bridge_threshold"s) {
                value = "!0";
            }
            if (opt_key == "overhangs_bridge_upper_layers"s) {
                value = "!2";
            }
            if (opt_key == "perimeters_hole"s) {
                value = "!0";
            }
            if (opt_key == "support_material_bottom_interface_layers"s) {
                value = "!0";
            }
            if (opt_key == "print_retract_length"s) {
                value = "!200";
            }
            if (opt_key == "print_retract_lift"s) {
                value = "!200";
            }
        }
        // nil-> disabled
        if (value.find("e+") != std::string::npos) {
            const ConfigOptionDef *def = PrintConfigDef::instance().get(opt_key);
            if (def && def->can_be_disabled) {
                ConfigOption *default_opt = def->default_value->clone();
                default_opt->deserialize(value);
                float max_value = std::numeric_limits<int32_t>::max() / 2;
                switch (default_opt->type()) {
                case coInt:
                case coPercent:
                case coFloat:
                case coFloatOrPercent:
                case coInts:
                case coPercents:
                case coFloats:
                case coFloatsOrPercents: {
                    for (size_t idx = 0; idx < default_opt->size(); idx++) {
                        if (std::abs(default_opt->get_float(idx)) > std::numeric_limits<int>::max() / 2) {
                            default_opt->set(*def->default_value, idx);
                            default_opt->set_enabled(false, idx);
                        }
                    }
                } break;
                default:;
                }
                value = default_opt->serialize();
                delete default_opt;
            }
        }
        // nil-> disabled
        if (value.find("nil") != std::string::npos) {
            const ConfigOptionDef *def = PrintConfigDef::instance().get(opt_key);
            if (def) {
                if (def->type != coString && def->type != coStrings) {
                    assert(def && def->can_be_disabled);
                    if (def && def->can_be_disabled) {
                        ConfigOption *default_opt = def->default_value->clone();
                        default_opt->set_enabled(false);
                        value = default_opt->serialize();
                        delete default_opt;
                    }
                }
            } else {
                // unknown key
                opt_key.clear();
            }
        }
    }
    //phony
    for (const auto &width_2_spacing : widths_2_spacings_for_phony_fix) {
        if (has(dict, width_2_spacing.first)) {
            const std::string &width_value = value();
            if (!has(dict, width_2_spacing.second)) {
                if (!width_value.empty()) {
                    // we have a width => phony spacing
                    dict[width_2_spacing.second] = {width_2_spacing.second, ""};
                } else {
                    // can't compute it... put 0
                    dict[width_2_spacing.second] = {width_2_spacing.second, "0"};
                }
            } else {
                const std::string &spacing_value = value();
                if (width_value.empty() && spacing_value.empty()) {
                    // all phony => set width to 0
                    dict[width_2_spacing.first] = {width_2_spacing.first, "0"};
                } else if (!width_value.empty() && !spacing_value.empty()) {
                    // no phony => set width to phony
                    dict[width_2_spacing.first] = {width_2_spacing.first, ""};
                }
            }
        } else if (has(dict, width_2_spacing.second)) {
            const std::string &spacing_value = value();
            if (!spacing_value.empty()) {
                // we have a spacing => phony width
                dict[width_2_spacing.first] = {width_2_spacing.first, ""};
            } else {
                    // can't compute it... put 0
                dict[width_2_spacing.first] = {width_2_spacing.first, "0"};
            }
        }
    }

}
} // namespace Handle_gacy_tools

void PrintConfigDef::handle_legacy_map(std::unordered_map<t_config_option_key, std::pair<t_config_option_key, std::string>> &dict, bool remove_unkown_keys)
{
    Handle_legacy_tools::_handle_legacy(dict, remove_unkown_keys);
}

void PrintConfigDef::handle_legacy_pair(t_config_option_key &opt_key, std::string &value, bool remove_unkown_keys)
{
    std::unordered_map<t_config_option_key, std::pair<t_config_option_key, std::string>> dict;
    dict[opt_key] = {opt_key, value};
    Handle_legacy_tools::_handle_legacy(dict, remove_unkown_keys);
    value = dict[opt_key].second;
    // erase opt_key last, as we need it to get the value
    opt_key = dict[opt_key].first;
}

// Called after a config is loaded as a whole.
// Perform composite conversions, for example merging multiple keys into one key.
// Don't convert single options here, implement such conversion in PrintConfigDef::handle_legacy() instead.
void PrintConfigDef::handle_legacy_composite(DynamicPrintConfig &config, std::map<t_config_option_key, std::string> &opt_deleted)
{
    bool old = true;
    if (config.has("print_version")) {
        std::string str_version = config.option<ConfigOptionString>("print_version")->value;
        old = str_version.size() < 4+1+7;
        old = old || str_version.substr(0,4) != "SUSI";
        assert(old || str_version[4] == '_');
        if (!old) {
            std::optional<Semver> version = Semver::parse(str_version.substr(5));
            if (version) {
                if (version->maj() <= 2 && version->min() <= 6) {
                    old = true;
                }
            } else {
                old = true;
            }
        }
    }
    if (old && config.has("bridge_angle") && config.get_float("bridge_angle") == 0 && config.is_enabled("bridge_angle")) {
        config.option("bridge_angle")->set_enabled(false);
    }
    bool enabled = !config.has("overhangs_width_speed") || config.is_enabled("overhangs_width_speed");
    if (old && config.has("overhangs_width_speed") && config.get_float("overhangs_width_speed") == 0 && config.is_enabled("overhangs_width_speed")) {
        config.option("overhangs_width_speed")->set_enabled(false);
    }
    if (old && config.has("overhangs_width") && config.has("overhangs_flow_ratio") && config.get_float("overhangs_width") == 0 && config.is_enabled("overhangs_flow_ratio")) {
        config.option("overhangs_flow_ratio")->set_enabled(false);
    }
    // enable_dynamic_overhang/fan_speeds
    std::map<t_config_option_key, std::string> useful_items;
    std::vector<t_config_option_key> to_erase;
    for (auto& [opt_key, value] : opt_deleted) {
        if (opt_key.find("overhang_fan_speed_") != std::string::npos) {
            useful_items[opt_key] = value;
            to_erase.push_back(opt_key);
        }
        if ("enable_dynamic_fan_speeds" == opt_key) {
            useful_items[opt_key] = value;
            to_erase.push_back(opt_key);
        }
        if (opt_key.find("overhang_speed_") != std::string::npos) {
            useful_items[opt_key] = value;
            to_erase.push_back(opt_key);
        }
        if ("enable_dynamic_overhang_speeds" == opt_key) {
            useful_items[opt_key] = value;
            to_erase.push_back(opt_key);
        }
    }
    for (const t_config_option_key &opt_key : to_erase) {
        opt_deleted.erase(opt_key);
    }
    if (useful_items.find("enable_dynamic_overhang_speeds") != useful_items.end() ||
        useful_items.find("overhang_speed_0") != useful_items.end()) {
        ConfigOptionBool enable_dynamic_overhang_speeds;
        if (useful_items.find("enable_dynamic_overhang_speeds") != useful_items.end())
            enable_dynamic_overhang_speeds.deserialize(useful_items["enable_dynamic_overhang_speeds"]);
        std::vector<ConfigOptionFloatOrPercent> values;
        values.resize(4);
        values[0].deserialize(useful_items["overhang_speed_0"]);
        values[1].deserialize(useful_items["overhang_speed_1"]);
        values[2].deserialize(useful_items["overhang_speed_2"]);
        values[3].deserialize(useful_items["overhang_speed_3"]);
        double external_perimeter_speed = config.get_computed_value("external_perimeter_speed");
        double max = external_perimeter_speed;
        double min = external_perimeter_speed;
        for (int x = 0; x < values.size(); ++x) {
            if (values[x].percent) {
                min = std::min(min, values[x].get_effective_value(external_perimeter_speed));
                max = std::max(max, values[x].get_effective_value(external_perimeter_speed));
            } else {
                min = std::min(min, values[x].value);
                max = std::max(max, values[x].value);
            }
        }
        //can't have both (min < external_perimeter_speed) & (max > external_perimeter_speed) at same time.
        if (min < external_perimeter_speed) {
            config.set_key_value("overhangs_speed", new ConfigOptionFloatOrPercent(min, false));
            max = external_perimeter_speed;
        } else if (max > external_perimeter_speed) {
            config.set_key_value("overhangs_speed", new ConfigOptionFloatOrPercent(max, false));
            min = external_perimeter_speed;
        } else {
            assert(min == max && min ==external_perimeter_speed);
        }
        ConfigOptionGraph opt;
        opt.set_can_be_disabled();
        // extract values
        Pointfs graph_curve;
        for (int x = 0; x < values.size(); ++x) {
            double speed = values[x].get_effective_value(external_perimeter_speed);
            speed = std::clamp(speed, min, max);
            double percent = (speed - min) / (max - min);
            if (min == external_perimeter_speed) {
                percent = 1 - percent;
            }
            graph_curve.push_back(Vec2d(x * 25, int(percent * 100)));
        }
        if (min == external_perimeter_speed) {
            graph_curve.push_back(Vec2d(100, 0));
        } else {
            graph_curve.push_back(Vec2d(100, 100));
        }
        opt.value = GraphData(graph_curve);
        if (useful_items.find("enable_dynamic_overhang_speeds") != useful_items.end())
            opt.set_enabled(enable_dynamic_overhang_speeds.value);
        config.set_key_value("overhangs_dynamic_speed", opt.clone());
    }
    if (useful_items.find("enable_dynamic_fan_speeds") != useful_items.end() ||
        useful_items.find("overhang_fan_speed_0") != useful_items.end()) {
        // note: there can be a enable_dynamic_fan_speeds and no overhang_fan_speed_X (if it's disabled)
        // note: there can be a overhang_fan_speed_0 but no overhang_fan_speed_1/2/3
        ConfigOptionBools enable_dynamic_fan_speeds;
        if(useful_items.find("enable_dynamic_fan_speeds") != useful_items.end())
            enable_dynamic_fan_speeds.deserialize(useful_items["enable_dynamic_fan_speeds"]);
        auto *external_perimeter_fan_speed = config.option<ConfigOptionInts>("external_perimeter_fan_speed");
        auto *perimeter_fan_speed = config.option<ConfigOptionInts>("perimeter_fan_speed");
        auto *default_fan_speed = config.option<ConfigOptionInts>("default_fan_speed");
        std::vector<ConfigOptionFloats> values;
        values.resize(4);
        if(useful_items.find("overhang_fan_speed_0") != useful_items.end())
            values[0].deserialize(useful_items["overhang_fan_speed_0"]);
        if(useful_items.find("overhang_fan_speed_1") != useful_items.end())
            values[1].deserialize(useful_items["overhang_fan_speed_1"]);
        if(useful_items.find("overhang_fan_speed_2") != useful_items.end())
            values[2].deserialize(useful_items["overhang_fan_speed_2"]);
        if(useful_items.find("overhang_fan_speed_3") != useful_items.end())
            values[3].deserialize(useful_items["overhang_fan_speed_3"]);
        ConfigOptionGraphs opt;
        opt.set_can_be_disabled();
        std::vector<GraphData> graph_data;
        //ensure same size
        if (enable_dynamic_fan_speeds.size() <  values[0].size()) {
            for (size_t extruder_id = 0; extruder_id < values[0].size(); extruder_id++) {
                enable_dynamic_fan_speeds.set_at(true, extruder_id);
            }
        }
        assert(enable_dynamic_fan_speeds.size() >= values[0].size());
        assert(values[0].size() >= values[1].size());
        assert(values[1].size() >= values[2].size());
        assert(values[2].size() >= values[3].size());
        const size_t overhang_fan_speed_size = enable_dynamic_fan_speeds.size();
        for (size_t extruder_id = 0; extruder_id < overhang_fan_speed_size; extruder_id++) {
            double default_value = 0;
            if (values[0].size() <= extruder_id) {
                assert(!enable_dynamic_fan_speeds.get_at(extruder_id));
                assert(values[0].size() == extruder_id);
                values[0].set_at(default_value, extruder_id);
            } else {
                default_value = values[0].get_at(extruder_id);
            }
            if (values[1].size() <= extruder_id) {
                assert(values[1].size() == extruder_id);
                values[1].set_at(default_value, extruder_id);
            } else {
                default_value = values[1].get_at(extruder_id);
            }
            if (values[2].size() <= extruder_id) {
                assert(values[2].size() == extruder_id);
                values[2].set_at(default_value, extruder_id);
            } else {
                default_value = values[2].get_at(extruder_id);
            }
            if (values[3].size() <= extruder_id) {
                assert(values[3].size() == extruder_id);
                values[3].set_at(default_value, extruder_id);
            }
        }
        // while there is a value
        for(int idx = 0 ;idx < enable_dynamic_fan_speeds.size(); ++idx) {
            // extract values
            Pointfs graph_curve;
            for (int x = 0; x < values.size(); ++x) {
                graph_curve.push_back(Vec2d(x*25, values[x].get_at(idx)));
            }
            if (external_perimeter_fan_speed && external_perimeter_fan_speed->is_enabled(idx)) {
                graph_curve.push_back(Vec2d(100, external_perimeter_fan_speed->get_at(idx)));
            } else if (perimeter_fan_speed && perimeter_fan_speed->is_enabled(idx)) {
                graph_curve.push_back(Vec2d(100, perimeter_fan_speed->get_at(idx)));
            } else if (default_fan_speed && default_fan_speed->is_enabled(idx)) {
                graph_curve.push_back(Vec2d(100, default_fan_speed->get_at(idx)));
            } else {
                graph_curve.push_back(Vec2d(100, graph_curve.back().y()));
            }
            graph_data.emplace_back(graph_curve);
        }
        // recreate fan speed graph
        opt.set(graph_data);
        for (int idx = 0; idx < enable_dynamic_fan_speeds.size(); ++idx) {
            opt.set_enabled(enable_dynamic_fan_speeds.get_at(idx), idx);
        }
        config.set_key_value("overhangs_dynamic_fan_speed", opt.clone());
    }
    
    if (auto it = opt_deleted.find("small_area_infill_flow_compensation"); it != opt_deleted.end()) {
        if (config.has("small_area_infill_flow_compensation_model")) {
            config.option("small_area_infill_flow_compensation_model")->set_enabled(it->second == "1");
        }
    }
    
    //if (config.has("thumbnails")) {
    //    std::string extention;
    //    if (config.has("thumbnails_format")) {
    //        if (const ConfigOptionDef* opt = config.def()->get("thumbnails_format")) {
    //            auto label = opt->enum_def->enum_to_label(config.option("thumbnails_format")->getInt());
    //            if (label.has_value())
    //                extention = *label;
    //        }
    //    }

    //    std::string thumbnails_str = config.opt_string("thumbnails");
    //    auto [thumbnails_list, errors] = GCodeThumbnails::make_and_check_thumbnail_list(thumbnails_str, extention);

    //    if (errors != enum_bitmask<ThumbnailError>()) {
    //        std::string error_str = "\n" + format("Invalid value provided for parameter %1%: %2%", "thumbnails", thumbnails_str);
    //        error_str += GCodeThumbnails::get_error_string(errors);
    //        throw BadOptionValueException(error_str);
    //    }

    //    if (!thumbnails_list.empty()) {
    //        const auto& extentions = ConfigOptionEnum<GCodeThumbnailsFormat>::get_enum_names();
    //        thumbnails_str.clear();
    //        for (const auto& [ext, size] : thumbnails_list)
    //            thumbnails_str += format("%1%x%2%/%3%, ", size.x(), size.y(), extentions[int(ext)]);
    //        thumbnails_str.resize(thumbnails_str.length() - 2);

    //        config.set_key_value("thumbnails", new ConfigOptionString(thumbnails_str));
    //    }
    //}
}

bool PrintConfigDef::is_defined(const t_config_option_key &opt_key) { return PrintConfigDef::instance().has(opt_key); }

// this is for extra things to add / modify from prusa that can't be handled otherwise.
// after handle_legacy
std::map<std::string,std::string> PrintConfigDef::from_prusa(t_config_option_key& opt_key, std::string& value, const DynamicConfig& all_conf) {
    std::map<std::string, std::string> output;
    if ("toolchange_gcode" == opt_key) {
        if (!value.empty() && value.find("T") == std::string::npos) {
            value = "T{next_extruder}\n" + value;
        }
    }
    if ("xy_size_compensation" == opt_key) {
        output["xy_inner_size_compensation"] = value;
    }
    if ("infill_anchor_max" == opt_key) {
        if(value == "0")
            output["infill_connection"] = "notconnected";
    }
    if ("first_layer_speed" == opt_key) {
        output["first_layer_min_speed"] = value;
        output["first_layer_infill_speed"] = value;
    }
    //dep between solid infill & infill accel are inverted for prusa.
    if ("infill_acceleration" == opt_key) {
        if (value == "0" && all_conf.get_float("solid_infill_acceleration") != 0) {
            value = std::to_string(all_conf.get_computed_value("default_acceleration"));
        }
    }
    if ("solid_infill_acceleration" == opt_key) {
        if (value == "0" && all_conf.get_float("infill_acceleration") != 0) {
            value = all_conf.get_float("infill_acceleration");
        }
    }
    if ("brim_type" == opt_key) {
        opt_key = "";
        if ("no_brim" == value) {
            output["brim_width"] = "0";
            output["brim_width_interior"] = "0";
        } else if ("outer_only" == value) {
            output["brim_width_interior"] = "0";
        } else if ("inner_only" == value) {
            output["brim_width_interior"] = all_conf.get_computed_value("brim_width");
            output["brim_width"] = "0";
        } else if ("outer_and_inner" == value) {
            output["brim_width_interior"] = all_conf.get_computed_value("brim_width");
        } else if ("auto_brim" == value) { //orca
            output["brim_width"] = "2";
            output["brim_width_interior"] = "0";
        } else if ("brim_ears" == value) { //orca
            output["brim_ears"] = "1";
        }
    }
    if ("thick_bridges" == opt_key) {
        opt_key = "bridge_type";
        if (value == "1") {
            value = "nozzle";
            output["bridge_overlap_min"] = "80%";
            output["bridge_overlap"] = "95%";
        } else {
            value = "flow";
            output["bridge_overlap_min"] = "60%";
            output["bridge_overlap"] = "75%";
        }
    }
    if ("support_material_contact_distance" == opt_key) {
        if ("0" == value) {
            output["support_material_contact_distance_type"] = "none";
        } else {
            output["support_material_contact_distance_type"] = "plane";
        }
    }
    if (opt_key == "seam_position") {
        if ("cost" == value ) { // eqauls to "near" == value || "nearest" == value
            output["seam_angle_cost"] = "50%";
            output["seam_travel_cost"] = "50%";
        } else if ("nearest" == value) {
            value = "cost";
            output["seam_angle_cost"] = "50%";
            output["seam_travel_cost"] = "50%";
        }
    }
    if ("bridge_type" == opt_key) { // seems like thick_bridge to 0
        if (value == "flow") {
            output["bridge_overlap_min"] = "60%";
            output["bridge_overlap"] = "75%";
        }
    }
    if ("first_layer_height" == opt_key) {
        if (!value.empty() && value.back() == '%') {
            // A first_layer_height isn't a % of layer_height but from nozzle_diameter now!
            // can't really convert right now, so put it at a safe value like 50%.
            value = "50%";
        }
    }
    if ("max_layer_height" == opt_key) {
        double min = 10;
        bool changed = false;
        std::vector<std::string> value_array;
        boost::split(value_array, value, boost::is_any_of(","), boost::token_compress_off);
        for (size_t i = 0; i < value_array.size(); ++i) {
            std::string &val = value_array[i];
            double dbl_val = std::atof(value.c_str());
            if (all_conf.has("nozzle_diameter")) {
                min = all_conf.option("nozzle_diameter")->get_float(i);
            }
            if (dbl_val > min) {
                if (dbl_val > 10) {
                    val += "%";
                    changed = true;
                } else {
                    val = to_string_nozero(min, 5);
                }
            }
        }
        if (changed) {
            value = "";
            for (const std::string &val : value_array) {
                if (!value.empty()) {
                    value += ",";
                }
                value += val;
            }
        }
    }
    if ("resolution" == opt_key && value == "0") {
        value = "0.0125";
    }
    // can't transfert from print config to printer config (unless there is both)
    if ("gcode_resolution" == opt_key && all_conf.has("nozzle_diameter")) {
        output["gcode_min_resolution"] = value;
    }
    if (("brim_width" == opt_key || "brim_width_interior" == opt_key) && all_conf.option("brim_separation") ) {
        // add brim_separation to brim_width & brim_width_interior
        float val = boost::lexical_cast<float>(value);
        if (val > 0) {
            val += all_conf.option("brim_separation")->get_float();
            value = to_string_nozero(val, 5);
        }
    }
    if ("fill_pattern" == opt_key && "alignedrectilinear" == value) {
        value = "rectilinear";
        output["fill_angle_increment"] = "90";
    } else if ("alignedrectilinear" == value) {
        value = "rectilinear";
    }
    if ("fan_always_on" == opt_key) {
        opt_key = "";
        //min_fan_speed is already converted to default_fan_speed, just has to deactivate it if not always_on
        if (value != "1") {
            if (all_conf.option("default_fan_speed")) {
                output["default_fan_speed"] = std::string("!") + all_conf.option("default_fan_speed")->serialize();
            } else {
                output["default_fan_speed"] = "!0";
            }
        }
    }
    if ("bridge_angle" == opt_key && "0" == value) {
        value = "!0";
    }
    if ("thumbnails_format" == opt_key) {
        // by default, no thumbnails_tag_format for png output
        if (value == "PNG")
            output["thumbnails_tag_format"] = "0";
    }
    if ("thumbnails" == opt_key && value.find('/') != std::string::npos) {
        // new (string from 2.7) .x./. , not old (Points before) type .x.
        auto [thumbnails_list, errors] = GCodeThumbnails::make_and_check_thumbnail_list_from_prusa(value);
        std::vector<Vec2d> pts;
        ConfigOptionEnum<GCodeThumbnailsFormat> opt_format;
        opt_format.value = thumbnails_list.empty() ? GCodeThumbnailsFormat::PNG : thumbnails_list.front().first;
        for (auto [format, pt] : thumbnails_list) {
            pts.push_back(pt);
        }
        value = ConfigOptionPoints(pts).serialize();
        // format (the first) is still set by prusa, no need to parse it.
        //output["thumbnails_format"] = opt_format.serialize();
    }
    /*
    if ("thumbnails" == opt_key) {
        //check if their format is inside the size
        if (value.find('/') != std::string::npos) {
            std::vector<std::string> sizes;
            boost::split(sizes, value, boost::is_any_of(","), boost::token_compress_off);
            value = "";
            std::string coma = "";
            size_t added = 0;
            for (std::string &size : sizes) {
                size_t pos = size.find('/');
                assert(pos != std::string::npos);
                if (pos != std::string::npos) {
                    assert(size.find('/', pos + 1) == std::string::npos);
                    value = value + coma + size.substr(0, pos);
                } else {
                    value = value + coma + size;
                }
                coma  = ",";
                added++;
                if (added >= 2)
                    break;
            }
            //if less than 2: add 0X0 until two.
            while (added < 2) {
                value = value + coma + "0x0";
                coma  = ",";
                added++;
            }
            // format (the first) is still set by prusa, no need to parse it.
        }
    }
*/
    
    // ---- custom gcode: ----
    static const std::vector<std::pair<std::string, std::string>> custom_gcode_replace =
        {{"[temperature]", "{temperature+extruder_temperature_offset}"},
         {"{temperature}", "{temperature+extruder_temperature_offset}"},
         {"[temperature[initial_tool]]", "{temperature[initial_tool]+extruder_temperature_offset[initial_tool]}"},
         {"{temperature[initial_tool]}", "{temperature[initial_tool]+extruder_temperature_offset[initial_tool]}"},
         {"[temperature[initial_extruder]]", "{temperature[initial_tool]+extruder_temperature_offset[initial_tool]}"},
         {"{temperature[initial_extruder]}", "{temperature[initial_tool]+extruder_temperature_offset[initial_tool]}"},
         {"[first_layer_temperature]", "{first_layer_temperature+extruder_temperature_offset}"},
         {"{first_layer_temperature}", "{first_layer_temperature+extruder_temperature_offset}"},
         {"[first_layer_temperature[initial_tool]]", "{first_layer_temperature[initial_tool]+extruder_temperature_offset[initial_tool]}"},
         {"[first_layer_temperature[initial_extruder]]", "{first_layer_temperature[initial_tool]+extruder_temperature_offset[initial_tool]}"},
         {"{first_layer_temperature[initial_tool]}", "{first_layer_temperature[initial_tool]+extruder_temperature_offset[initial_tool]}"},
         {"{first_layer_temperature[initial_extruder]}", "{first_layer_temperature[initial_tool]+extruder_temperature_offset[initial_tool]}"},
         {"!is_nil(", "is_enabled("},
         {"is_nil(", "!is_enabled("}};

    static const std::set<t_config_option_key> custom_gcode_keys =
        {"template_custom_gcode", "toolchange_gcode", "before_layer_gcode",
         "between_objects_gcode", "end_gcode",        "layer_gcode",
         "feature_gcode",         "start_gcode",      "color_change_gcode",
         "pause_print_gcode",     "toolchange_gcode", "end_filament_gcode",
         "start_filament_gcode"};
    if (custom_gcode_keys.find(opt_key) != custom_gcode_keys.end()) {
        // check & replace
        for (auto &entry : custom_gcode_replace) {
            boost::replace_all(value, entry.first, entry.second);
        }
    }

    return output;
}

//keys that needs to go through from_prusa before beeing deserialized.
const std::unordered_set<std::string> prusa_import_to_review_keys =
{
    "thumbnails"
};


template<typename CONFIG_CLASS>
void _convert_from_prusa(CONFIG_CLASS& conf, const DynamicPrintConfig& global_config, bool with_phony) {
    //void convert_from_prusa(DynamicPrintConfig& conf, const DynamicPrintConfig & global_config) {
    //void convert_from_prusa(ModelConfigObject& conf, const DynamicPrintConfig& global_config) {
    std::map<std::string, std::string> results;
    for (const t_config_option_key& opt_key : conf.keys()) {
        const ConfigOption* opt = conf.option(opt_key);
        std::string serialized = opt->serialize();
        std::string key = opt_key;
        std::map<std::string, std::string> result = PrintConfigDef::from_prusa(key, serialized, global_config);
        if (key != opt_key) {
            conf.erase(opt_key);
        }
        if (!key.empty() && serialized != opt->serialize()) {
            ConfigOption* opt_new = opt->clone();
            opt_new->deserialize(serialized);
            conf.set_key_value(key, opt_new);
        }
        results.insert(result.begin(), result.end());
    }
    for (auto entry : results) {
        const ConfigOptionDef* def = PrintConfigDef::instance().get(entry.first);
        if (def) {
            ConfigOption* opt_new = def->default_value.get()->clone();
            opt_new->deserialize(entry.second); // note: deserialize don't set phony, only the ConfigBase::set_deserialize*
            conf.set_key_value(entry.first, opt_new);
        }
    }

    // set phony entries
    if (with_phony) {
        for (auto & [opt_key_width, opt_key_spacing] : Handle_legacy_tools::widths_2_spacings_for_phony_fix) {
            // if prusa has defined a width, or if the conf has a default spacing that need to be overwritten
            if (conf.option(opt_key_width) != nullptr || conf.option(opt_key_spacing) != nullptr) {
                ConfigOption *opt_new = PrintConfigDef::instance().get(opt_key_spacing)->default_value.get()->clone();
                opt_new->deserialize(""); // note: deserialize don't set phony, only the ConfigBase::set_deserialize*
                opt_new->set_phony(true);
                conf.set_key_value(opt_key_spacing, opt_new);
            }
        }
    }
}

void DynamicPrintConfig::convert_from_prusa(bool with_phony) {
    _convert_from_prusa<DynamicPrintConfig>(*this, *this, with_phony);
}
void ModelConfig::convert_from_prusa(const DynamicPrintConfig& global_config, bool with_phony) {
    _convert_from_prusa<ModelConfig>(*this, global_config, with_phony);
}


template<typename CONFIG_CLASS>
void _deserialize_maybe_from_prusa(const std::map<t_config_option_key, std::string> settings,
                                           CONFIG_CLASS &                             config,
                                           const DynamicPrintConfig &                 global_config,
                                           ConfigSubstitutionContext &                config_substitutions,
                                           bool                                       with_phony,
                                           bool                                       check_prusa)
{
    std::unordered_map<t_config_option_key, std::pair<t_config_option_key, std::string>> dict_opt;
    std::map<t_config_option_key, std::string> deleted_keys;
    std::unordered_map<t_config_option_key, std::pair<t_config_option_key, std::string>> unknown_keys;
    for (const auto &[my_key, value] : settings) {
        dict_opt[my_key] = {my_key, value};
    }
    PrintConfigDef::handle_legacy_map(dict_opt);
    const ConfigDef *def = config.def();
    for (const auto &[key, pair] : dict_opt) {
        try {
            const t_config_option_key &opt_key = key;
            const std::string &opt_value = pair.second;
            if (!pair.first.empty()) {
                if (!def->has(pair.first) ||
                    (check_prusa && prusa_import_to_review_keys.find(pair.first) != prusa_import_to_review_keys.end())) {
                    unknown_keys[key] = {key, opt_value/*should be old value, before handle_legacy*/}; 
                } else {
                    config.set_deserialize(pair.first, opt_value, config_substitutions);
                    if (auto it = settings.find(key); config_substitutions.rule == ForwardCompatibilitySubstitutionRule::Enable && it != settings.end() && it->second != opt_value) {
                        const ConfigOptionDef *optdef = def->get(pair.first);
                        if (optdef != nullptr) {
                            ConfigSubstitution substitution(optdef, settings.at(key), ConfigOptionUniquePtr(config.option(pair.first)->clone()));
                            substitution.old_name = key;
                            config_substitutions.add(std::move(substitution));
                        } else {
                            config_substitutions.add(ConfigSubstitution(key, opt_value));
                        }
                    }
                }
            } else {
                deleted_keys[key] = opt_value/*should be old value, before handle_legacy*/;
            }
        } catch (UnknownOptionException & /* e */) {
            // log & ignore
            if (config_substitutions.rule != ForwardCompatibilitySubstitutionRule::Disable)
                config_substitutions.add(ConfigSubstitution(key, pair.second/*should be old value, before handle_legacy*/));
            assert(false);
        } catch (BadOptionValueException &e) {
            if (config_substitutions.rule == ForwardCompatibilitySubstitutionRule::Disable)
                throw e;
            // log the error
            const ConfigDef *def = config.def();
            if (def == nullptr)
                throw e;
            const ConfigOptionDef *optdef = def->get(key);
            config_substitutions.emplace(optdef,std::string(pair.second/*should be old value, before handle_legacy*/), ConfigOptionUniquePtr(optdef->default_value->clone()));
        }
    }
    // TODO: add composite substitutions into config_substitutions
    config.handle_legacy_composite(deleted_keys);
    if (config_substitutions.rule == ForwardCompatibilitySubstitutionRule::Enable) {
        for (const auto &[key, value] : deleted_keys) {
            if (key != "threads") {
                config_substitutions.add(ConfigSubstitution(key, value));
            }
        }
    }
    // from prusa: try again with from_prusa before handle_legacy
    if (check_prusa) {
        dict_opt.clear();
        for (auto &[key, pair] : unknown_keys) {
            std::map<t_config_option_key, std::string> result = PrintConfigDef::from_prusa(pair.first, pair.second, global_config);
            if (!pair.first.empty())
                dict_opt[pair.first] = { pair.first, pair.second };
            for (auto &[k, v] : result) {
                dict_opt[k] = { k, v };
            }
        }
        PrintConfigDef::handle_legacy_map(dict_opt);
        for (auto &[key, pair] : dict_opt) {
            bool substitution_handle = false;
            if (!pair.first.empty()) {
                if (!def->has(pair.first)) {
                    if (config_substitutions.rule != ForwardCompatibilitySubstitutionRule::Disable) {
                        config_substitutions.add(ConfigSubstitution(key, pair.second));
                    }
                } else {
                    try {
                        config.set_deserialize(pair.first, pair.second, config_substitutions);
                    } catch (BadOptionValueException &e) {
                        if (config_substitutions.rule == ForwardCompatibilitySubstitutionRule::Disable)
                            throw e;
                        // log the error
                        if (def == nullptr)
                            throw e;
                        const ConfigOptionDef *optdef = def->get(key);
                        if (optdef != nullptr) {
                            config_substitutions.emplace(optdef, std::string(pair.second), ConfigOptionUniquePtr(optdef->default_value->clone()));
                        } else {
                            config_substitutions.add(ConfigSubstitution(key, pair.second));
                        }
                    }
                }
            } else if (def != nullptr && config_substitutions.rule != ForwardCompatibilitySubstitutionRule::Disable) {
                const ConfigOptionDef *optdef = def->get(key);
                if (optdef != nullptr) {
                    config_substitutions.emplace(optdef, std::string(pair.second), ConfigOptionUniquePtr(optdef->default_value->clone()));
                } else {
                    config_substitutions.add(ConfigSubstitution(key, pair.second));
                }
            }
        }
    } else {
        for (const auto& [key, pair] : unknown_keys) {
            if (config_substitutions.rule != ForwardCompatibilitySubstitutionRule::Disable) {
                config_substitutions.add(ConfigSubstitution(key, pair.second));
            }
        }
    }

    // set phony entries
    if (with_phony) {
        const ConfigDef *def = config.def();
        for (auto & [opt_key_width, opt_key_spacing] : Handle_legacy_tools::widths_2_spacings_for_phony_fix) {
            const ConfigOption *opt_width = config.option(opt_key_width);
            const ConfigOption *opt_spacing = config.option(opt_key_spacing);
            if (opt_width && opt_spacing) {
                // if the config has a default spacing that need to be overwritten (if the width wasn't deserialized as phony)
                if (settings.find(opt_key_spacing) == settings.end()) {
                    if (opt_width->is_phony()) {
                        if (opt_spacing->is_phony()) {
                            ConfigOption *opt_new = opt_spacing->clone();
                            opt_new->set_phony(false);
                            config.set_key_value(opt_key_spacing, opt_new);
                        }
                    } else {
                        if (!opt_spacing->is_phony()) {
                            ConfigOption *opt_new = opt_spacing->clone();
                            opt_new->set_phony(true);
                            config.set_key_value(opt_key_spacing, opt_new);
                        }
                    }
                } else {
                    //spacing exist in the config, make sure one if phony
                    if (opt_spacing->is_phony() && opt_width->is_phony()) {
                        ConfigOption *opt_new = opt_width->clone();
                        opt_new->set_phony(false);
                        config.set_key_value(opt_key_width, opt_new);
                    }
                    if (!opt_spacing->is_phony() && !opt_width->is_phony()) {
                        ConfigOption *opt_new = opt_width->clone();
                        opt_new->set_phony(true);
                        config.set_key_value(opt_key_width, opt_new);
                    }
                }
                assert(config.option(opt_key_width)->is_phony() != config.option(opt_key_spacing)->is_phony());
            }
        }
    }
}
void deserialize_maybe_from_prusa(std::map<t_config_option_key, std::string> settings,
                                  ModelConfig &                              config,
                                  const DynamicPrintConfig &                 global_config,
                                  ConfigSubstitutionContext &                config_substitutions,
                                  bool                                       with_phony,
                                  bool                                       check_prusa)
{
    _deserialize_maybe_from_prusa(settings, config, global_config, config_substitutions, with_phony, check_prusa);
}
void deserialize_maybe_from_prusa(std::map<t_config_option_key, std::string> settings,
                                  DynamicPrintConfig &                       config,
                                  ConfigSubstitutionContext &                config_substitutions,
                                  bool                                       with_phony,
                                  bool                                       check_prusa)
{
    _deserialize_maybe_from_prusa(settings, config, config, config_substitutions, with_phony, check_prusa);
}


std::unordered_set<std::string> prusa_export_to_remove_keys = {
"allow_empty_layers",
"arc_fitting_ignore_holes",
"arc_fitting_resolution",
"arc_fitting_tolerance",
"autospeed_min_thin_flow",
"avoid_crossing_not_first_layer",
"avoid_crossing_top",
"avoid_travel_island",
"avoid_travel_island_weight",
"between_objects_gcode_before_move",
"bridge_fill_pattern",
"bridge_precision",
"bridge_overlap",
"bridge_overlap_min",
"bridge_type",
"bridged_infill_margin",
"brim_acceleration",
"brim_ears_detection_length",
"brim_ears_max_angle",
"brim_ears_pattern",
"brim_ears",
"brim_inside_holes",
"brim_per_object",
"brim_speed",
"brim_width_interior",
"chamber_temperature",
"complete_objects_one_skirt",
"complete_objects_sort",
"curve_smoothing_angle_concave",
"curve_smoothing_angle_convex",
"curve_smoothing_cutoff_dist",
"curve_smoothing_precision",
"default_speed",
"enforce_full_fill_volume",
// "exact_last_layer_height",
"external_infill_margin",
"external_perimeter_cut_corners",
"external_perimeter_extrusion_spacing",
"external_perimeter_extrusion_change_odd_layers",
"external_perimeter_fan_speed",
"external_perimeter_overlap",
"external_perimeters_first_force",
"external_perimeters_hole",
"external_perimeters_nothole",
"seam_slope_type",
"seam_slope_min_height",
"seam_slope_max_length",
"extra_perimeters_below_area",
"extra_perimeters_count",
"extra_perimeters_odd_layers",
"extruder_clearance",
"extruder_extrusion_multiplier_speed",
"extruder_fan_offset",
"extruder_temperature_offset",
"extrusion_spacing",
"fan_kickstart",
"fan_percentage",
"fan_printer_min_speed",
"fan_speedup_overhangs",
"fan_speedup_time",
"feature_gcode",
"filament_cooling_zone_pause",
"filament_custom_variables",
"filament_dip_extraction_speed",
"filament_dip_insertion_speed",
"filament_enable_toolchange_part_fan",
"filament_enable_toolchange_temp",
"filament_fill_top_flow_ratio",
"filament_first_layer_flow_ratio",
"filament_max_speed",
"filament_max_wipe_tower_speed",
"filament_melt_zone_pause",
"filament_max_overlap",
"filament_pressure_advance",
"filament_bridge_pa",
"filament_bridge_internal_pa",
"filament_brim_pa",
"filament_external_perimeter_pa",
"filament_first_layer_pa",
"filament_first_layer_pa_over_raft",
"filament_gap_fill_pa",
"filament_infill_pa",
"filament_ironing_pa",
"filament_overhangs_pa",
"filament_perimeter_pa",
"filament_solid_infill_pa",
"filament_support_material_pa",
"filament_support_material_interface_pa",
"filament_thin_walls_pa",
"filament_top_solid_infill_pa",
"filament_travel_pa",
"filament_retract_lift_before_travel",
"filament_shrink",
"filament_skinnydip_distance",
"filament_toolchange_part_fan_speed",
"filament_toolchange_temp",
"filament_use_fast_skinnydip",
"filament_use_skinnydip",
"filament_wipe_advanced_pigment",
"fill_aligned_z",
"fill_angle_increment",
"fill_angle_cross",
"fill_angle_follow_model",
"fill_angle_template",
"fill_smooth_distribution",
"fill_smooth_width",
"fill_top_flow_ratio",
"fill_top_flow_ratio",
"first_layer_extruder",
"first_layer_extrusion_spacing",
"first_layer_infill_extrusion_width",
"first_layer_infill_extrusion_spacing",
"first_layer_flow_ratio",
"first_layer_infill_speed",
"first_layer_min_speed",
"first_layer_size_compensation_layers",
"first_layer_size_compensation_no_collapse",
"first_layer_strong_start",
"gcode_ascii",
"gcode_command_buffer",
"gcode_min_length",
"gcode_min_resolution",
"gap_fill_acceleration",
"gap_fill_extension",
"gap_fill_fan_speed",
"gap_fill_flow_match_perimeter",
"gap_fill_last",
"gap_fill_infill",
"gap_fill_min_area",
"gap_fill_max_width",
"gap_fill_min_length",
"gap_fill_min_width",
"gap_fill_no_overhang",
"gap_fill_overlap",
"gap_fill_perimeter",
"gcode_filename_illegal_char",
"gcode_precision_e",
"gcode_precision_xyz",
"hole_size_compensation",
"hole_size_threshold",
"infill_connection",
"infill_connection_bottom",
"infill_connection_bridge",
"infill_connection_solid",
"infill_connection_top",
"infill_dense_algo",
"infill_dense",
"infill_extrusion_change_odd_layers",
"infill_extrusion_spacing",
"infill_fan_speed",
"infill_filled_bottom",
"infill_filled_solid",
"infill_filled_top",
"init_z_rotate",
"internal_bridge_acceleration",
"internal_bridge_expansion",
"internal_bridge_fan_speed",
"internal_bridge_min_width",
"internal_bridge_speed",
"ironing_acceleration",
"ironing_angle",
"lift_min",
"max_gcode_per_second",
"max_speed_reduction",
"milling_after_z",
"milling_cutter",
"milling_diameter",
"milling_extra_size",
"milling_offset",
"milling_post_process",
"milling_speed",
"milling_toolchange_end_gcode",
"milling_toolchange_start_gcode",
"milling_z_lift",
"milling_z_offset",
"min_width_top_surface",
"model_precision",
"no_perimeter_unsupported_algo",
"object_gcode",
"only_one_perimeter_top_other_algo",
"only_one_perimeter_top",
"only_one_perimeter_first_layer",
"over_bridge_flow_ratio",
"overhangs_acceleration",
"overhangs_bridge_threshold",
"overhangs_bridge_upper_layers",
"overhangs_dynamic_flow",
"overhangs_extrusion_spacing",
"overhangs_fan_speed",
"overhangs_flow_ratio",
"overhangs_max_slope",
"overhangs_reverse_threshold",
"overhangs_reverse",
"overhangs_speed_enforce",
"overhangs_type",
"overhangs_width_speed",
"parallel_islands",
"parallel_objects_step",
"parallel_objects_step_max_z",
"perimeter_bonding",
"perimeter_extrusion_change_odd_layers",
"perimeter_extrusion_spacing",
"perimeter_direction",
"perimeter_fan_speed",
"perimeter_loop_seam",
"perimeter_loop",
"perimeter_overlap",
"perimeter_reverse",
"perimeter_round_corners",
"perimeters_hole",
"priming_position",
"print_bed_temperature",
"print_extrusion_multiplier",
"print_first_layer_bed_temperature",
"print_first_layer_temperature",
"print_custom_variables",
"print_retract_length",
"print_retract_lift",
"print_temperature",
"printer_custom_variables",
"printhost_client_cert",
"printhost_client_cert_password",
"raft_contact_distance_type",
"raft_interface_layer_height",
"raft_layer_height",
"region_gcode",
"remaining_times_type",
"resolution_internal",
"retract_lift_first_layer",
"retract_lift_top",
"retract_lift_before_travel",
"retract_restart_toolchange_on_perimeter",
"retract_restart_wipe_toolchange",
"seam_angle_cost",
"seam_gap",
"seam_gap_external",
"solid_over_perimeters",
"filament_seam_gap", // filament override
"filament_seam_gap_external", // filament override
"seam_notch_all",
"seam_notch_angle",
"seam_notch_inner",
"seam_notch_outer",
"seam_travel_cost",
"seam_visibility",
"slice_merge_dent",
"slice_merge_min_width",
"skirt_brim",
"skirt_distance_from_brim",
"skirt_extrusion_width",
"small_area_infill_flow_compensation_model",
"small_perimeter_max_length",
"small_perimeter_min_length",
"solid_fill_pattern",
"solid_infill_extrusion_change_odd_layers",
"solid_infill_extrusion_spacing",
"solid_infill_fan_speed",
"solid_infill_overlap",
"start_gcode_manual",
"solid_infill_below_layer_area",
"solid_infill_below_width",
"support_material_angle_height",
"support_material_acceleration",
"support_material_contact_distance_type",
"support_material_fan_speed",
"support_material_interface_acceleration",
"support_material_interface_angle",
"support_material_interface_angle_increment",
"support_material_interface_fan_speed",
"support_material_interface_layer_height",
"support_material_bottom_interface_expansion",
"support_material_bottom_interface_pattern",
"support_material_layer_height",
"temperature_heat_speed",
"thin_perimeters_all",
"thin_perimeters",
"thin_walls_acceleration",
"thin_walls_merge",
"thin_walls_min_width",
"thin_walls_overlap",
"thin_walls_speed",
"thumbnails_color",
"thumbnails_custom_color",
"thumbnails_end_file",
"thumbnails_tag_format",
"thumbnails_with_bed",
"thumbnails_with_support",
"time_cost",
"time_estimation_compensation",
"time_start_gcode",
"time_toolchange",
"tool_name",
"top_fan_speed",
"top_infill_extrusion_spacing",
"top_solid_infill_overlap",
"travel_acceleration",
"travel_deceleration_use_target",
"wipe_advanced_algo",
"wipe_advanced_multiplier",
"wipe_advanced_nozzle_melted_volume",
"wipe_advanced",
"wipe_extra_perimeter",
"wipe_inside_depth",
"wipe_inside_end",
"wipe_inside_start",
"wipe_lift",
"wipe_lift_length",
"wipe_min",
"wipe_only_crossing",
"wipe_return",
"wipe_speed",
"filament_temperature_heat_speed", // filament override
"filament_wipe_extra_perimeter", // filament override
"filament_wipe_inside_depth", // filament override
"filament_wipe_inside_end", // filament override
"filament_wipe_inside_start", // filament override
"filament_wipe_lift", // filament override
"filament_wipe_lift_length", // filament override
"filament_wipe_min", // filament override
"filament_wipe_only_crossing", // filament override
"filament_wipe_return", // filament override
"filament_wipe_speed", // filament override
"wipe_tower_extrusion_width",
"wipe_tower_rest_in_middle",
"wipe_tower_speed",
"wipe_tower_wipe_starting_speed",
"xy_size_compensation",
"xy_inner_size_compensation",
"z_step",

"print_version",
};

std::unordered_set<std::string> prusa_export_to_change_keys =
{
"default_fan_speed", // used to convert to min_fan_speed & fan_always_on
"overhangs_width",
"overhangs_speed",
};

void add_to_prusa_export_to_remove_keys(std::string &opt_key) {
    prusa_export_to_remove_keys.insert(opt_key);
}

std::map<std::string, std::string> PrintConfigDef::to_prusa(t_config_option_key& opt_key, std::string& value, const DynamicConfig& all_conf) {
    std::map<std::string, std::string> new_entries;

    //looks if it's to be removed, or have to be transformed
    if (prusa_export_to_remove_keys.find(opt_key) != prusa_export_to_remove_keys.end()) {
        assert((all_conf.def()->get(opt_key)->mode & comPrusa) == 0);
        opt_key = "";
        value = "";
        return new_entries;
    }
    if (!opt_key.empty() && prusa_export_to_change_keys.find(opt_key) == prusa_export_to_change_keys.end()) {
        auto mode = all_conf.def()->get(opt_key)->mode;
        assert( (mode & comPrusa) == comPrusa || mode == coNone);
    }
    if (opt_key.find("_pattern") != std::string::npos) {
        if ("smooth" == value || "smoothtriple" == value || "smoothhilbert" == value || "rectiwithperimeter" == value || "scatteredrectilinear" == value || "rectilineargapfill" == value || "sawtooth" == value) {
            value = "rectilinear";
        } else if ("concentricgapfill" == value) {
            value = "concentric";
        } else if ("monotonicgapfill" == value) {
            value = "monotonic";
        }
        if (all_conf.has("fill_angle_increment") && ((int(all_conf.option("fill_angle_increment")->get_float())-90)%180) == 0 && "rectilinear" == value
            && ("fill_pattern" == opt_key || "top_fill_pattern" == opt_key)) {
            value = "alignedrectilinear";
        }
        if ("support_material_top_interface_pattern" == opt_key) {
            opt_key = "support_material_interface_pattern";
        }
    } else if ("seam_position" == opt_key) {
        if ("cost" == value) {
            value = "nearest";
        }else if ("allrandom" == value) {
            value = "random";
        }else if ("contiguous" == value) {
            value = "aligned";
        }
    } else if ("first_layer_size_compensation" == opt_key) {
        opt_key = "elefant_foot_compensation";
        if (!value.empty()) {
            if (value[0] == '-') {
                value = value.substr(1);
            } else {
                value = "0";
            }
        }
    } else if ("elephant_foot_min_width" == opt_key) {
        opt_key = "elefant_foot_min_width";
    } else if("first_layer_acceleration" == opt_key || "first_layer_acceleration_over_raft" == opt_key) {
        if (value.find("%") != std::string::npos) {
            // can't support %, so we uese the default accel a baseline for half-assed conversion
            value = std::to_string(all_conf.option(opt_key)->get_effective_value(all_conf.get_computed_value("default_acceleration")));
        }
    } else if ("infill_acceleration" == opt_key || "solid_infill_acceleration" == opt_key || "top_solid_infill_acceleration" == opt_key
        || "bridge_acceleration" == opt_key || "default_acceleration" == opt_key || "perimeter_acceleration" == opt_key
        || "overhangs_speed" == opt_key || "ironing_speed" == opt_key || "perimeter_speed" == opt_key 
        || "infill_speed" == opt_key || "bridge_speed" == opt_key || "support_material_speed" == opt_key
        || "max_print_speed" == opt_key
        ) {
        // remove '%', change 0 with different meanings
        if (value.find("%") != std::string::npos) {
            if (value == "100%")
                value = "0";
            else
                value = std::to_string(all_conf.get_computed_value(opt_key));
        }
        // infill_acceleration & solid_infill_acceleration dep are inverted
        if ("infill_acceleration" == opt_key && value == "0") {
            value = std::to_string(all_conf.get_computed_value("solid_infill_acceleration"));
        } else if ("solid_infill_acceleration" == opt_key && value == "0") {
            value = std::to_string(all_conf.get_computed_value("default_acceleration"));
        }
    } else if ("gap_fill_speed" == opt_key && all_conf.has("gap_fill_enabled") && !all_conf.option<ConfigOptionBool>("gap_fill_enabled")->value) {
        value = "0";
    } else if ("bridge_flow_ratio" == opt_key && all_conf.has("bridge_flow_ratio")) {
        value = to_string_nozero(all_conf.option<ConfigOptionPercent>("bridge_flow_ratio")->get_effective_value(1), 5);
    //} else if ("overhangs_width" == opt_key) {
    //    opt_key = "overhangs";
    //    if ((!value.empty() && value.front() == '!') || !all_conf.is_enabled("overhangs_width_speed")) {
    //        value = "0";
    //    } else {
    //        value = "1";
    //    }
    } else if ("support_material_contact_distance_top" == opt_key) {
        opt_key = "support_material_contact_distance";
        //default : get the top value or 0.2 if a %
        if (value.find("%") != std::string::npos)
            value = "0.2";
        try { //avoid most useless cheks and multiple corners cases with this try catch
            SupportZDistanceType dist_type = all_conf.option<ConfigOptionEnum<SupportZDistanceType>>("support_material_contact_distance_type")->value;
            if (SupportZDistanceType::zdNone == dist_type) {
                value = "0";
            } else {
                double val = all_conf.option<ConfigOptionFloatOrPercent>("support_material_contact_distance_top")->get_effective_value(all_conf.option<ConfigOptionFloats>("nozzle_diameter")->get_at(0));
                if (SupportZDistanceType::zdFilament == dist_type) { // not exact but good enough effort
                    val += all_conf.option<ConfigOptionFloats>("nozzle_diameter")->get_at(0);
                    val -= all_conf.get_computed_value("layer_height", 0);
                }
                value = to_string_nozero(val, 5);
            }
        }
        catch (...) {
        }
    } else if ("support_material_contact_distance_bottom" == opt_key) {
            opt_key = "support_material_bottom_contact_distance";
            //default : get the top value or 0.2 if a %
            if (value.find("%") != std::string::npos)
                value = "0.2";
            try { //avoid most useless cheks and multiple corners cases with this try catch
                SupportZDistanceType dist_type = all_conf.option<ConfigOptionEnum<SupportZDistanceType>>("support_material_contact_distance_type")->value;
                if (SupportZDistanceType::zdNone == dist_type) {
                    value = "0";
                } else {
                    double val = all_conf.option<ConfigOptionFloatOrPercent>("support_material_contact_distance_bottom")->get_effective_value(all_conf.option<ConfigOptionFloats>("nozzle_diameter")->get_at(0));
                    if (SupportZDistanceType::zdFilament == dist_type) { // not exact but good enough effort
                        val += all_conf.option<ConfigOptionFloats>("nozzle_diameter")->get_at(0);
                        val -= all_conf.get_computed_value("layer_height", 0);
                    }
                    value = to_string_nozero(val, 5);
                }
            }
            catch (...) {
            }
        } else if ("gcode_flavor" == opt_key) {
        if ("sprinter" == value)
            value = "reprap";
        else if ("lerdge" == value)
            value = "marlin";
        else if ("klipper" == value)
            value = "reprap";
    } else if ("host_type" == opt_key) {
        if ("klipper" == value)
            value = "octoprint";
    } else if (opt_key.find("extrusion_width") != std::string::npos) {
        if (std::set<std::string>{"extrusion_width", "first_layer_extrusion_width", "perimeter_extrusion_width", "external_perimeter_extrusion_width", 
            "infill_extrusion_width", "solid_infill_extrusion_width", "top_infill_extrusion_width", "support_material_extrusion_width"}.count(opt_key) > 0) {
            const ConfigOptionFloatOrPercent* opt = all_conf.option<ConfigOptionFloatOrPercent>(opt_key);
            if (opt->is_phony() || opt->percent) {
                if (opt->percent) {
                    ConfigOptionFloat opt_temp{ opt->get_effective_value(all_conf.option<ConfigOptionFloats>("nozzle_diameter")->get_at(0)) };
                    value = opt_temp.serialize();
                } else {
                    //bypass the phony kill switch from Config::opt_serialize
                    value = opt->serialize();
                }
            }
        }
    }
    if ("infill_anchor_max" == opt_key) {
        //it's infill_anchor == 0 that disable it for prusa
        if (all_conf.opt_serialize("infill_connection") == "notconnected") {
            value = "0";
        }
    }
    if ("brim_width" == opt_key) {
        double brim_width_interior = all_conf.get_computed_value("brim_width_interior");
        if (value == "0" && brim_width_interior == 0)
            new_entries["brim_type"] = "no_brim";
        else if (value != "0" && brim_width_interior == 0)
            new_entries["brim_type"] = "outer_only";
        else if (value == "0" && brim_width_interior != 0)
            new_entries["brim_type"] = "inner_only";
        else if (value != "0" && brim_width_interior != 0)
            new_entries["brim_type"] = "outer_and_inner";
    }
    if ("output_format" == opt_key) {
        opt_key = "sla_archive_format";
    }
    if ("host_type" == opt_key) {
        if ("klipper" == value || "mpmdv2" == value || "monoprice" == value) value = "octoprint";
    }
    if ("fan_below_layer_time" == opt_key) {
        if (value.find('.') != std::string::npos)
            value = value.substr(0, value.find('.'));
    }
    if ("bed_custom_texture" == opt_key || "Bed custom texture" == opt_key) {
        value = Slic3r::find_full_path(value, value).generic_string();
    }
    if ("default_fan_speed" == opt_key) {
        if (!value.empty() && value.front() == '!') {
            new_entries["fan_always_on"] = "0";
        } else {
            new_entries["fan_always_on"] = "1";
        }
        opt_key = "min_fan_speed";
        value = std::to_string(std::max(all_conf.option("fan_printer_min_speed")->get_float(),
                                        all_conf.option("default_fan_speed")->get_float()));
    }
    if ("bridge_fan_speed" == opt_key) {
        if (!value.empty() && value.front() == '!') {
            value = std::to_string(all_conf.option("fan_printer_min_speed")->get_float());
        }
    }
    if ("travel_ramping_lift" == opt_key && "1" == value) {
        // also add travel_max_lift from retract_lift & same from filament
        new_entries["travel_ramping_lift"] = all_conf.option("retract_lift")->serialize();
        new_entries["filament_travel_ramping_lift"] = all_conf.option("filament_retract_lift")->serialize();
    }

    // compute max & min height from % to flat value
    if ("min_layer_height" == opt_key || "max_layer_height" == opt_key) {
        if ("max_layer_height" == opt_key && !value.empty() && value.front() == '!') {
            value = "0";
        } else {
            ConfigOptionFloats computed_opt;
            const ConfigOptionFloatsOrPercents *current_opt = all_conf.option<ConfigOptionFloatsOrPercents>(opt_key);
            const ConfigOptionFloats *nozzle_diameters = all_conf.option<ConfigOptionFloats>("nozzle_diameter");
            assert(current_opt && nozzle_diameters);
            assert(current_opt->size() == nozzle_diameters->size());
            for (int i = 0; i < current_opt->size(); i++) {
                computed_opt.set_at(current_opt->get_effective_value(nozzle_diameters->get_at(i), i), i);
            }
            assert(computed_opt.size() == nozzle_diameters->size());
            value = computed_opt.serialize();
        }
    }
    if ("arc_fitting" == opt_key && "bambu" == value) {
        value = "emit_center";
    }
    if ("wipe_tower_brim_width" == opt_key && value.find("%") != std::string::npos) {
        const ConfigOptionFloatOrPercent *current_opt = all_conf.option<ConfigOptionFloatOrPercent>(opt_key);
        assert(current_opt && current_opt->percent);
        const ConfigOptionFloats *nozzle_diameters = all_conf.option<ConfigOptionFloats>("nozzle_diameter");
        value = std::to_string(current_opt->get_effective_value(nozzle_diameters->get_at(0)));
    }

    if ("thumbnails" == opt_key) {
    // add format to thumbnails
        const ConfigOptionEnum<GCodeThumbnailsFormat> *format_opt = all_conf.option<ConfigOptionEnum<GCodeThumbnailsFormat>>("thumbnails_format");
        std::string format = format_opt->serialize();
        std::vector<std::string> sizes;
        boost::split(sizes, value, boost::is_any_of(","), boost::token_compress_off);
        value = "";
        std::string coma = "";
        for (std::string &size : sizes) {
            //if first or second dimension is 0: ignore.
            if (size.find("0x") == 0 || size.find("x0") + 2 == size.size())
                continue;
            assert(size.find('/') == std::string::npos);
            value = value + coma + size + std::string("/") + format;
            coma = ",";
        }
    }

    // disabled
    if (!value.empty() && value.front() == '!') {
        // ----- -1 -> disabled -----
        static const std::set<t_config_option_key> minus_1_is_disabled = {"overhangs_bridge_threshold",
              "overhangs_bridge_upper_layers", "perimeters_hole", "support_material_bottom_interface_layers"};
        // ---- filament override ------
        if (boost::starts_with(opt_key, "filament_")) {
            std::string extruder_key = opt_key.substr(strlen("filament_"));
            if (PrintConfigDef::instance().filament_override_option_keys().find(extruder_key) !=
                PrintConfigDef::instance().filament_override_option_keys().end()) {
                value = "nil";
            }
        }
        if (boost::starts_with(opt_key, "material_ow_")) {
            std::string normal_key = opt_key.substr(strlen("material_ow_"));
            if (PrintConfigDef::instance().material_overrides_option_keys().find(opt_key) !=
                PrintConfigDef::instance().material_overrides_option_keys().end()) {
                value = "nil";
            }
        }
        if ("idle temperature" == opt_key) {
            value = "nil";
        } else if (minus_1_is_disabled.find(opt_key) != minus_1_is_disabled.end()) {
            value = "-1";
        }
    }
    
    if ("bridge_angle" == opt_key && !value.empty() && value.front() == '!') {
        value = "0";
    }

    // ---- custom gcode: ----
    static const std::vector<std::pair<std::string, std::string>> custom_gcode_replace =
        {{"extruder_temperature_offset[initial_extruder]", "0"}, {"extruder_temperature_offset", "0"}};
    static const std::set<t_config_option_key> custom_gcode_keys =
        {"template_custom_gcode", "toolchange_gcode", "before_layer_gcode",
         "between_objects_gcode", "end_gcode",        "layer_gcode",
         "feature_gcode",         "start_gcode",      "color_change_gcode",
         "pause_print_gcode",     "toolchange_gcode", "end_filament_gcode",
         "start_filament_gcode"};
    if (custom_gcode_keys.find(opt_key) != custom_gcode_keys.end()) {
        // check & replace
        for (auto &entry : custom_gcode_replace) {
            boost::replace_all(value, entry.first, entry.second);
        }
    }
    // --end-- custom gcode: --end--


    return new_entries;
}

PrintConfigDef& private_instance()
{
    // function-initialised instead of legacy cpp-file-init to avoid static init order fiasco & dll boundaries issues
    static PrintConfigDef static_instance;
    return static_instance;
}

// init singleton
PrintConfigDef& PrintConfigDef::instance_mutable()
{
    PrintConfigDef &def = private_instance();
    if (def.is_finalized())
        throw std::logic_error("PrintConfigDef::instance_mutable() called after PrintConfigDef::finalize()");
    return def;
}

const PrintConfigDef& PrintConfigDef::instance()
{
    const PrintConfigDef &def = private_instance();
//    if (!def.is_finalized())
//        throw std::logic_error("PrintConfigDef::instance() called before PrintConfigDef::finalize()");
    return def;
}

DynamicPrintConfig DynamicPrintConfig::full_print_config()
{
	return DynamicPrintConfig((const PrintRegionConfig&)FullPrintConfig::defaults());
}

DynamicPrintConfig::DynamicPrintConfig(const StaticPrintConfig& rhs) : DynamicConfig(rhs, rhs.keys_ref())
{
}

DynamicPrintConfig* DynamicPrintConfig::new_from_defaults_keys(const std::vector<std::string> &keys)
{
    auto *out = new DynamicPrintConfig();
    out->apply_only(FullPrintConfig::defaults(), keys);
    return out;
}

const ConfigOption *MultiPtrPrintConfig::optptr(const t_config_option_key &opt_key) const
{
    for (ConfigBase *conf : storages) {
#ifdef _DEBUG
        assert(conf->exists());
#endif
        const ConfigOption *opt = conf->optptr(opt_key);
        if (opt)
            return opt;
    }
    return nullptr;
}
ConfigOption *MultiPtrPrintConfig::optptr(const t_config_option_key &opt_key, bool create)
{
    assert(!create);
    for (ConfigBase *conf : storages) {
#ifdef _DEBUG
        assert(conf->exists());
#endif
        ConfigOption *opt = conf->optptr(opt_key);
        if (opt)
            return opt;
    }
    return nullptr;
}
t_config_option_keys MultiPtrPrintConfig::keys() const
{
    assert(false);
    // shouldn't need ot call that
    t_config_option_keys keys;
    for (ConfigBase *conf : storages) {
#ifdef _DEBUG
        assert(conf->exists());
#endif
        append(keys, conf->keys());
    }
    return keys;
}

PrinterTechnology printer_technology(const ConfigBase& cfg)
{
    const ConfigOptionEnum<PrinterTechnology>* opt = cfg.option<ConfigOptionEnum<PrinterTechnology>>("printer_technology");

    if (opt) return opt->value;

    const ConfigOptionBool* export_opt = cfg.option<ConfigOptionBool>("export_sla");
    if (export_opt && export_opt->get_bool()) return ptSLA;

    export_opt = cfg.option<ConfigOptionBool>("export_gcode");
    if (export_opt && export_opt->get_bool()) return ptFFF;

    return ptUnknown;
}

OutputFormat output_format(const ConfigBase& cfg)
{
    std::cerr << "Detected technology " << printer_technology(cfg) << "\n";
    if (printer_technology(cfg) == ptFFF) return ofGCode;
    const ConfigOptionEnum<OutputFormat>* opt = cfg.option<ConfigOptionEnum<OutputFormat>>("output_format");
    if (opt) return opt->value;

    return ofUnknown;
}

void DynamicPrintConfig::normalize_fdm()
{
    if (this->has("extruder")) {
        int extruder = this->option("extruder")->get_int();
        this->erase("extruder");
        if (extruder != 0) {
            if (!this->has("infill_extruder"))
                this->option<ConfigOptionInt>("infill_extruder", true)->value = (extruder);
            if (!this->has("perimeter_extruder"))
                this->option<ConfigOptionInt>("perimeter_extruder", true)->value = (extruder);
            // Don't propagate the current extruder to support.
            // For non-soluble supports, the default "0" extruder means to use the active extruder,
            // for soluble supports one certainly does not want to set the extruder to non-soluble.
            // if (!this->has("support_material_extruder"))
            //     this->option("support_material_extruder", true)->setInt(extruder);
            // if (!this->has("support_material_interface_extruder"))
            //     this->option("support_material_interface_extruder", true)->setInt(extruder);
        }
    }
    if (this->has("first_layer_extruder"))
        this->erase("first_layer_extruder");

    if (this->has("wipe_tower_extruder")) {
        // If invalid, replace with 0.
        int extruder = this->opt<ConfigOptionInt>("wipe_tower_extruder")->value;
        int num_extruders = this->opt<ConfigOptionFloats>("nozzle_diameter")->size();
        if (extruder < 0 || extruder > num_extruders)
            this->opt<ConfigOptionInt>("wipe_tower_extruder")->value = 0;
    }

    if (!this->has("solid_infill_extruder") && this->has("infill_extruder"))
        this->option<ConfigOptionInt>("solid_infill_extruder", true)->value = (this->option("infill_extruder")->get_int());

    if (this->has("spiral_vase") && this->opt<ConfigOptionBool>("spiral_vase", true)->value) {
        {
            // this should be actually done only on the spiral layers instead of all
            auto* opt = this->opt<ConfigOptionBools>("retract_layer_change", true);
            opt->set(std::vector<uint8_t>(opt->size(), false));  // set all values to false
            // Disable retract on layer change also for filament overrides.
            auto* opt_n = this->opt<ConfigOptionBools>("filament_retract_layer_change", true);
            opt_n->set(std::vector<uint8_t>(opt_n->size(), false));  // Set all values to false.
        }
        {
            this->opt<ConfigOptionInt>("top_solid_layers", true)->value = 0;
            this->opt<ConfigOptionPercent>("fill_density", true)->value = 0;
            this->opt<ConfigOptionEnumGeneric>("perimeter_generator", true)->value = (int)PerimeterGeneratorType::Classic;
            this->opt<ConfigOptionBool>("support_material", true)->value = false;
            this->opt<ConfigOptionInt>("solid_over_perimeters")->value = 0;
            this->opt<ConfigOptionInt>("support_material_enforce_layers")->value = 0;
            // this->opt<ConfigOptionBool>("exact_last_layer_height", true)->value = false;
            this->opt<ConfigOptionBool>("infill_dense", true)->value = false;
            this->opt<ConfigOptionBool>("extra_perimeters", true)->value = false;
            this->opt<ConfigOptionFloatOrPercent>("extra_perimeters_below_area")->value = 0;
            this->opt<ConfigOptionInt>("extra_perimeters_count")->value = 0;
            this->opt<ConfigOptionBool>("extra_perimeters_odd_layers", true)->value = false;
            this->opt<ConfigOptionBool>("extra_perimeters_on_overhangs", true)->value = false;
            this->opt<ConfigOptionBool>("overhangs_reverse", true)->value = false; 
            this->opt<ConfigOptionBool>("perimeter_reverse", true)->value = false; 
        }
    }

// merill : why?
//    if (auto* opt_gcode_resolution = this->opt<ConfigOptionFloat>("gcode_resolution", false); opt_gcode_resolution)
//        // Resolution will be above 1um.
//        opt_gcode_resolution->value = std::max(opt_gcode_resolution->value, 0.001);

    if (auto *opt_min_bead_width = this->opt<ConfigOptionFloat>("min_bead_width", false); opt_min_bead_width)
        opt_min_bead_width->value = std::max(opt_min_bead_width->value, 0.001);
    if (auto *opt_wall_transition_length = this->opt<ConfigOptionFloat>("wall_transition_length", false); opt_wall_transition_length)
        opt_wall_transition_length->value = std::max(opt_wall_transition_length->value, 0.001);

    if (auto *opt_min_bead_width = this->opt<ConfigOptionFloat>("min_bead_width", false); opt_min_bead_width)
        opt_min_bead_width->value = std::max(opt_min_bead_width->value, 0.001);
    if (auto *opt_wall_transition_length = this->opt<ConfigOptionFloat>("wall_transition_length", false); opt_wall_transition_length)
        opt_wall_transition_length->value = std::max(opt_wall_transition_length->value, 0.001);
}

void  handle_legacy_sla(DynamicPrintConfig& config)
{
    for (std::string corr : {"relative_correction", "material_correction"}) {
        if (config.has(corr)) {
            if (std::string corr_x = corr + "_x"; !config.has(corr_x)) {
                auto* opt = config.opt<ConfigOptionFloat>(corr_x, true);
                opt->value = config.opt<ConfigOptionFloats>(corr)->get_at(0);
            }

            if (std::string corr_y = corr + "_y"; !config.has(corr_y)) {
                auto* opt = config.opt<ConfigOptionFloat>(corr_y, true);
                opt->value = config.opt<ConfigOptionFloats>(corr)->get_at(0);
            }

            if (std::string corr_z = corr + "_z"; !config.has(corr_z)) {
                auto* opt = config.opt<ConfigOptionFloat>(corr_z, true);
                opt->value = config.opt<ConfigOptionFloats>(corr)->get_at(1);
            }
        }
    }
}

void DynamicPrintConfig::set_num_extruders(unsigned int num_extruders)
{
    for (const std::string &key : PrintConfigDef::instance().extruder_option_keys()) {
        if (key == "default_filament_profile")
            // Don't resize this field, as it is presented to the user at the "Dependencies" page of the Printer profile and we don't want to present
            // empty fields there, if not defined by the system profile.
            continue;
        auto *opt = this->option(key, false);
        assert(opt != nullptr && opt->is_vector());
        if (opt != nullptr && opt->is_vector()) {
            auto default_opt_it = PrintConfigDef::instance().options.find(key);
            assert(default_opt_it != PrintConfigDef::instance().options.end());
            static_cast<ConfigOptionVectorBase *>(opt)->resize(num_extruders, default_opt_it->second.default_value.get());
        }
    }
}

void DynamicPrintConfig::set_num_milling(unsigned int num_milling)
{
    for (const std::string& key : PrintConfigDef::instance().milling_option_keys()) {
        auto* opt = this->option(key, false);
        assert(opt != nullptr);
        assert(opt->is_vector());
        if (opt != nullptr && opt->is_vector()) {
            auto default_opt_it = PrintConfigDef::instance().options.find(key);
            assert(default_opt_it != PrintConfigDef::instance().options.end());
            static_cast<ConfigOptionVectorBase *>(opt)->resize(num_milling, default_opt_it->second.default_value.get());
        }
    }
}

std::string DynamicPrintConfig::validate()
{
    // Full print config is initialized from the defaults.
    const ConfigOption *opt = this->option("printer_technology", false);
    auto printer_technology = (opt == nullptr) ? ptFFF : static_cast<PrinterTechnology>(dynamic_cast<const ConfigOptionEnumGeneric*>(opt)->value);
    switch (printer_technology) {
    case ptFFF:
    {
        FullPrintConfig fpc;
        fpc.apply(*this, true);
        // Verify this print options through the FullPrintConfig.
        return Slic3r::validate(fpc);
    }
    default:
        //FIXME no validation on SLA data?
        return std::string();
    }
}

template<typename TYPE>
const TYPE* find_option(const t_config_option_key &opt_key,const  DynamicPrintConfig* default_config, const std::vector<const DynamicPrintConfig*> &other_config) {
    const TYPE* option = default_config->option<TYPE>(opt_key);
    if (option)
        return option;
    for (const DynamicPrintConfig* conf : other_config) {
        option = conf->option<TYPE>(opt_key);
        if (option)
            return option;
    }
    return nullptr;
}

const DynamicPrintConfig* DynamicPrintConfig::update_phony(const std::vector<const DynamicPrintConfig*> config_collection, bool exclude_default_extrusion /*= false*/) {
    const DynamicPrintConfig* something_changed = nullptr;
    //update width/spacing links
    const char* widths[] = { "", "external_perimeter_", "perimeter_", "infill_", "solid_infill_", "top_infill_", "support_material_", "first_layer_", "first_layer_infill_", "skirt_" };
    for (size_t i = exclude_default_extrusion?1:0; i < sizeof(widths) / sizeof(widths[i]); ++i) {
        std::string key_width(widths[i]);
        key_width += "extrusion_width";
        std::string key_spacing(widths[i]);
        key_spacing += "extrusion_spacing";
        ConfigOptionFloatOrPercent* width_option = this->option<ConfigOptionFloatOrPercent>(key_width);
        ConfigOptionFloatOrPercent* spacing_option = this->option<ConfigOptionFloatOrPercent>(key_spacing);
        if (width_option && spacing_option){
            const DynamicPrintConfig* returned_value;
            if (!spacing_option->is_phony() && width_option->is_phony())
                returned_value = value_changed(key_spacing, config_collection);
             else
                returned_value = value_changed(key_width, config_collection);
             if (something_changed == nullptr)
                something_changed = returned_value;
        }
    }

    return something_changed;
}

//note: width<-> spacing conversion is done via float, so max 6-7 digit of precision.
const DynamicPrintConfig* DynamicPrintConfig::value_changed(const t_config_option_key& opt_key, const std::vector<const DynamicPrintConfig*> config_collection) {
    if (opt_key == "layer_height") {
        const ConfigOptionFloat* layer_height_option = find_option<ConfigOptionFloat>("layer_height", this, config_collection);
        //if bad layer height, slip to be able to go to the check part without outputing exceptions.
        if (layer_height_option && layer_height_option->value < EPSILON)
            return nullptr;
        if (this->update_phony(config_collection) != nullptr)
            return this;
        return nullptr;
    }
    if (opt_key == "filament_max_overlap" || opt_key == "perimeter_overlap" ||
        opt_key == "external_perimeter_overlap" || opt_key == "solid_infill_overlap" ||
        opt_key == "top_solid_infill_overlap") {
        if (this->option("extrusion_width")) {
            if (this->update_phony(config_collection) != nullptr) {
                return this;
            }
        }
        return nullptr;
    }

    bool something_changed = false;
    // width -> spacing
    if (opt_key.find("extrusion_spacing") != std::string::npos) {
        const ConfigOptionFloats* nozzle_diameter_option = find_option<ConfigOptionFloats>("nozzle_diameter", this, config_collection);
        const ConfigOptionFloat* layer_height_option = find_option<ConfigOptionFloat>("layer_height", this, config_collection);
        ConfigOptionFloatOrPercent* spacing_option = this->option<ConfigOptionFloatOrPercent>(opt_key);
        if (layer_height_option && spacing_option && nozzle_diameter_option) {
            //compute spacing with current height and change the width
            double max_nozzle_diameter = 0;
            for (double dmr : nozzle_diameter_option->get_values())
                max_nozzle_diameter = std::max(max_nozzle_diameter, dmr);
            double spacing_value = spacing_option->get_effective_value(max_nozzle_diameter);
            float overlap_ratio = 1;
            const ConfigOptionPercents* filament_max_overlap_option = find_option<ConfigOptionPercents>("filament_max_overlap", this, config_collection);
            if (filament_max_overlap_option) overlap_ratio = filament_max_overlap_option->get_effective_value(1., 0);
            Flow flow = Flow::new_from_spacing(spacing_value, max_nozzle_diameter,layer_height_option->value, overlap_ratio, false);
            //test for valid height. If too high, revert to round shape
            if (spacing_value > 0 && flow.height() > spacing_value / (1 - (1. - 0.25 * PI) * flow.spacing_ratio())) {
                flow = flow.with_width(spacing_value / (1 - (1. - 0.25 * PI) * flow.spacing_ratio()));
                flow = flow.with_height(flow.width());
            }
            if (opt_key == "extrusion_spacing") {
                ConfigOptionFloatOrPercent* width_option = this->option<ConfigOptionFloatOrPercent>("extrusion_width");
                if (width_option) {
                    width_option->set_phony(true);
                    spacing_option->set_phony(false);
                    if (spacing_value == 0)
                        width_option->value = 0;
                    else
                        width_option->value = (spacing_option->percent) ? std::round(100 * flow.width() / max_nozzle_diameter) : (std::round(flow.width() * 10000) / 10000);
                    width_option->percent = spacing_option->percent;
                    something_changed = true;
                }
            }
            if (opt_key == "first_layer_extrusion_spacing") {
                ConfigOptionFloatOrPercent* width_option = this->option<ConfigOptionFloatOrPercent>("first_layer_extrusion_width");
                if (width_option) {
                    width_option->set_phony(true);
                    spacing_option->set_phony(false);
                    if (spacing_value == 0)
                        width_option->value = 0;
                    else
                        width_option->value = (spacing_option->percent) ? std::round(100 * flow.width() / max_nozzle_diameter) : (std::round(flow.width() * 10000) / 10000);
                    width_option->percent = spacing_option->percent;
                    something_changed = true;
                }
            }
            if (opt_key == "first_layer_infill_extrusion_spacing") {
                ConfigOptionFloatOrPercent* width_option = this->option<ConfigOptionFloatOrPercent>("first_layer_infill_extrusion_width");
                if (width_option) {
                    width_option->set_phony(true);
                    spacing_option->set_phony(false);
                    if (spacing_value == 0)
                        width_option->value = 0;
                    else
                        width_option->value = (spacing_option->percent) ? std::round(100 * flow.width() / max_nozzle_diameter) : (std::round(flow.width() * 10000) / 10000);
                    width_option->percent = spacing_option->percent;
                    something_changed = true;
                }
            }
            if (opt_key == "perimeter_extrusion_spacing") {
                const ConfigOptionPercent* perimeter_overlap_option = find_option<ConfigOptionPercent>("perimeter_overlap", this, config_collection);
                ConfigOptionFloatOrPercent* width_option = this->option<ConfigOptionFloatOrPercent>("perimeter_extrusion_width");
                if (width_option && perimeter_overlap_option) {
                    width_option->set_phony(true);
                    spacing_option->set_phony(false);
                    if(spacing_value == 0)
                        width_option->value = 0;
                    else {
                        float spacing_ratio = (std::min(flow.spacing_ratio(), float(perimeter_overlap_option->get_effective_value(1))));
                        flow = flow.with_width( spacing_option->get_effective_value(max_nozzle_diameter) + layer_height_option->value * (1. - 0.25 * PI) * spacing_ratio);
                        width_option->value = (spacing_option->percent) ? std::round(100 * flow.width() / max_nozzle_diameter) : (std::round(flow.width() * 10000) / 10000);
                    }
                    width_option->percent = spacing_option->percent;
                    something_changed = true;
                }
            }
            if (opt_key == "external_perimeter_extrusion_spacing") {
                const ConfigOptionPercent* external_perimeter_overlap_option = find_option<ConfigOptionPercent>("external_perimeter_overlap", this, config_collection);
                ConfigOptionFloatOrPercent* width_option = this->option<ConfigOptionFloatOrPercent>("external_perimeter_extrusion_width");
                if (width_option && external_perimeter_overlap_option) {
                    width_option->set_phony(true);
                    spacing_option->set_phony(false);
                    if (spacing_value == 0)
                        width_option->value = 0;
                    else {
                        float spacing_ratio = (std::min(flow.spacing_ratio() / 2, float(external_perimeter_overlap_option->get_effective_value(0.5))));
                        flow = flow.with_width(spacing_option->get_effective_value(max_nozzle_diameter) + layer_height_option->value * (1. - 0.25 * PI) * spacing_ratio);
                        width_option->value = (spacing_option->percent) ? std::round(100 * flow.width() / max_nozzle_diameter) : (std::round(flow.width() * 10000) / 10000);
                    }
                    width_option->percent = spacing_option->percent;
                    something_changed = true;
                }
            }
            if (opt_key == "infill_extrusion_spacing") {
                ConfigOptionFloatOrPercent* width_option = this->option<ConfigOptionFloatOrPercent>("infill_extrusion_width");
                if (width_option) {
                    width_option->set_phony(true);
                    spacing_option->set_phony(false);
                    if (spacing_value == 0)
                        width_option->value = 0;
                    else
                        width_option->value = (spacing_option->percent) ? std::round(100 * flow.width() / max_nozzle_diameter) : (std::round(flow.width() * 10000) / 10000);
                    width_option->percent = spacing_option->percent;
                    something_changed = true;
                }
            }
            if (opt_key == "solid_infill_extrusion_spacing") {
                const ConfigOptionPercent* solid_infill_overlap_option = find_option<ConfigOptionPercent>("solid_infill_overlap", this, config_collection);
                ConfigOptionFloatOrPercent* width_option = this->option<ConfigOptionFloatOrPercent>("solid_infill_extrusion_width");
                if (width_option) {
                    width_option->set_phony(true);
                    spacing_option->set_phony(false);
                    if (spacing_value == 0)
                        width_option->value = 0;
                    else {
                        float spacing_ratio = (std::min(flow.spacing_ratio(), float(solid_infill_overlap_option->get_effective_value(1))));
                        flow = flow.with_width(spacing_option->get_effective_value(max_nozzle_diameter) + layer_height_option->value * (1. - 0.25 * PI) * spacing_ratio);
                        width_option->value = (spacing_option->percent) ? std::round(100 * flow.width() / max_nozzle_diameter) : (std::round(flow.width() * 10000) / 10000);
                    }
                    width_option->percent = spacing_option->percent;
                    something_changed = true;
                }
            }
            if (opt_key == "top_infill_extrusion_spacing") {
                const ConfigOptionPercent* top_solid_infill_overlap_option = find_option<ConfigOptionPercent>("top_solid_infill_overlap", this, config_collection);
                ConfigOptionFloatOrPercent* width_option = this->option<ConfigOptionFloatOrPercent>("top_infill_extrusion_width");
                if (width_option) {
                    width_option->set_phony(true);
                    spacing_option->set_phony(false);
                    if (spacing_value == 0)
                        width_option->value = 0;
                    else {
                        float spacing_ratio = (std::min(flow.spacing_ratio(), float(top_solid_infill_overlap_option->get_effective_value(1))));
                        flow = flow.with_width(spacing_option->get_effective_value(max_nozzle_diameter) + layer_height_option->value * (1. - 0.25 * PI) * spacing_ratio);
                        width_option->value = (spacing_option->percent) ? std::round(100 * flow.width() / max_nozzle_diameter) : (std::round(flow.width() * 10000) / 10000);
                    }
                    width_option->percent = spacing_option->percent;
                    something_changed = true;
                }
            }
            /*if (opt_key == "support_material_extrusion_spacing") {
                if (spacing_option->percent)
                    this->set_key_value("support_material_extrusion_width", new ConfigOptionFloatOrPercent(std::round(100 * flow.width / max_nozzle_diameter), true));
                else
                    this->set_key_value("support_material_extrusion_width", new ConfigOptionFloatOrPercent(std::round(flow.width * 10000) / 10000, false));
                something_changed = true;
            }
            if (opt_key == "skirt_extrusion_spacing") {
                if (spacing_option->percent)
                    this->set_key_value("skirt_extrusion_width", new ConfigOptionFloatOrPercent(std::round(100 * flow.width / max_nozzle_diameter), true));
                else
                    this->set_key_value("skirt_extrusion_width", new ConfigOptionFloatOrPercent(std::round(flow.width * 10000) / 10000, false));
                something_changed = true;
            }*/
        }
    }
    if (opt_key.find("extrusion_width") != std::string::npos) {
        const ConfigOptionFloats* nozzle_diameter_option = find_option<ConfigOptionFloats>("nozzle_diameter", this, config_collection);
        const ConfigOptionFloat* layer_height_option = find_option<ConfigOptionFloat>("layer_height", this, config_collection);
        ConfigOptionFloatOrPercent* default_width_option = this->option<ConfigOptionFloatOrPercent>("extrusion_width");
        ConfigOptionFloatOrPercent* width_option = this->option<ConfigOptionFloatOrPercent>(opt_key);
        float overlap_ratio = 1;
        const ConfigOptionPercents* filament_max_overlap_option = find_option<ConfigOptionPercents>("filament_max_overlap", this, config_collection);
        if (filament_max_overlap_option) overlap_ratio = filament_max_overlap_option->get_effective_value(1., 0);
        if (layer_height_option && width_option && nozzle_diameter_option) {
            //compute spacing with current height and change the width
            float max_nozzle_diameter = 0;
            for (double dmr : nozzle_diameter_option->get_values())
                max_nozzle_diameter = std::max(max_nozzle_diameter, (float)dmr);
            ConfigOptionFloatOrPercent* spacing_option = nullptr;
            try {
                if (opt_key == "extrusion_width") {
                    spacing_option = this->option<ConfigOptionFloatOrPercent>("extrusion_spacing");
                    if (width_option) {
                            width_option->set_phony(false);
                            spacing_option->set_phony(true);
                            if (width_option->value == 0)
                                spacing_option->value = 0;
                            else {
                                Flow flow = Flow::new_from_config_width(FlowRole::frPerimeter, width_option->value == 0 ? *default_width_option : *width_option, *spacing_option, max_nozzle_diameter, layer_height_option->value, overlap_ratio, 0);
                                if (flow.width() < flow.height()) flow.with_height(flow.width());
                                spacing_option->value = (width_option->percent) ? std::round(100 * flow.spacing() / max_nozzle_diameter) : (std::round(flow.spacing() * 10000) / 10000);
                            }
                            spacing_option->percent = width_option->percent;
                            something_changed = true;
                    }
                }
                if (opt_key == "first_layer_extrusion_width") {
                    spacing_option = this->option<ConfigOptionFloatOrPercent>("first_layer_extrusion_spacing");
                    if (width_option) {
                            width_option->set_phony(false);
                            spacing_option->set_phony(true);
                            if (width_option->value == 0)
                                spacing_option->value = 0;
                            else {
                                Flow flow = Flow::new_from_config_width(FlowRole::frPerimeter, 
                                    width_option->value == 0 ? *default_width_option : *width_option, *spacing_option, 
                                    max_nozzle_diameter, layer_height_option->value, overlap_ratio, 0);
                                if (flow.width() < flow.height()) flow.with_height(flow.width());
                                spacing_option->value = (width_option->percent) ? std::round(100 * flow.spacing() / max_nozzle_diameter) : (std::round(flow.spacing() * 10000) / 10000);
                            }
                            spacing_option->percent = width_option->percent;
                            something_changed = true;
                    }
                }
                if (opt_key == "first_layer_infill_extrusion_width") {
                    spacing_option = this->option<ConfigOptionFloatOrPercent>("first_layer_infill_extrusion_spacing");
                    if (width_option) {
                            width_option->set_phony(false);
                            spacing_option->set_phony(true);
                            if (width_option->value == 0)
                                spacing_option->value = 0;
                            else {
                                Flow flow = Flow::new_from_config_width(FlowRole::frPerimeter, 
                                    width_option->value == 0 ? *default_width_option : *width_option, *spacing_option, 
                                    max_nozzle_diameter, layer_height_option->value, overlap_ratio, 0);
                                if (flow.width() < flow.height()) flow.with_height(flow.width());
                                spacing_option->value = (width_option->percent) ? std::round(100 * flow.spacing() / max_nozzle_diameter) : (std::round(flow.spacing() * 10000) / 10000);
                            }
                            spacing_option->percent = width_option->percent;
                            something_changed = true;
                    }
                }
                if (opt_key == "perimeter_extrusion_width") {
                    const ConfigOptionPercent* perimeter_overlap_option = find_option<ConfigOptionPercent>("perimeter_overlap", this, config_collection);
                    spacing_option = this->option<ConfigOptionFloatOrPercent>("perimeter_extrusion_spacing");
                    if (width_option && perimeter_overlap_option) {
                        width_option->set_phony(false);
                        spacing_option->set_phony(true);
                        if (width_option->value == 0)
                            spacing_option->value = 0;
                        else {
                            Flow flow = Flow::new_from_config_width(FlowRole::frExternalPerimeter, 
                                width_option->value == 0 ? *default_width_option : *width_option,  *spacing_option, 
                                max_nozzle_diameter, layer_height_option->value, 
                                std::min(overlap_ratio, (float)perimeter_overlap_option->get_effective_value(1)), 0);
                            if (flow.width() < flow.height()) flow = flow.with_height(flow.width());
                            spacing_option->value = (width_option->percent) ? std::round(100 * flow.spacing() / max_nozzle_diameter) : (std::round(flow.spacing() * 10000) / 10000);
                        }
                        spacing_option->percent = width_option->percent;
                        something_changed = true;
                    }
                }
                if (opt_key == "external_perimeter_extrusion_width") {
                    const ConfigOptionPercent* external_perimeter_overlap_option = find_option<ConfigOptionPercent>("external_perimeter_overlap", this, config_collection);
                    spacing_option = this->option<ConfigOptionFloatOrPercent>("external_perimeter_extrusion_spacing");
                    if (width_option && external_perimeter_overlap_option) {
                        width_option->set_phony(false);
                        spacing_option->set_phony(true);
                        if (width_option->value == 0)
                            spacing_option->value = 0;
                        else {
                            Flow ext_perimeter_flow = Flow::new_from_config_width(FlowRole::frPerimeter, 
                                width_option->value == 0 ? *default_width_option : *width_option, *spacing_option, 
                                max_nozzle_diameter, layer_height_option->value, 
                                std::min(overlap_ratio * 0.5f, float(external_perimeter_overlap_option->get_effective_value(0.5))), 0);
                            if (ext_perimeter_flow.width() < ext_perimeter_flow.height()) ext_perimeter_flow = ext_perimeter_flow.with_height(ext_perimeter_flow.width());
                            spacing_option->value = (width_option->percent) ? std::round(100 * ext_perimeter_flow.spacing() / max_nozzle_diameter) : (std::round(ext_perimeter_flow.spacing() * 10000) / 10000);
                        }
                        spacing_option->percent = width_option->percent;
                        something_changed = true;
                    }
                }
                if (opt_key == "infill_extrusion_width") {
                    spacing_option = this->option<ConfigOptionFloatOrPercent>("infill_extrusion_spacing");
                    if (width_option) {
                        width_option->set_phony(false);
                        spacing_option->set_phony(true);
                        if (width_option->value == 0)
                            spacing_option->value = 0;
                        else {
                            Flow flow = Flow::new_from_config_width(FlowRole::frInfill, width_option->value == 0 ? *default_width_option : *width_option, *spacing_option, max_nozzle_diameter, layer_height_option->value, overlap_ratio, 0);
                            if (flow.width() < flow.height()) flow = flow.with_height(flow.width());
                            spacing_option->value = (width_option->percent) ? std::round(100 * flow.spacing() / max_nozzle_diameter) : (std::round(flow.spacing() * 10000) / 10000);
                        }
                        spacing_option->percent = width_option->percent;
                        something_changed = true;
                    }
                }
                if (opt_key == "solid_infill_extrusion_width") {
                    const ConfigOptionPercent* solid_infill_overlap_option = find_option<ConfigOptionPercent>("solid_infill_overlap", this, config_collection);
                    spacing_option = this->option<ConfigOptionFloatOrPercent>("solid_infill_extrusion_spacing");
                    if (width_option) {
                        width_option->set_phony(false);
                        spacing_option->set_phony(true);
                        if (width_option->value == 0)
                            spacing_option->value = 0;
                        else {
                            Flow flow = Flow::new_from_config_width(FlowRole::frSolidInfill, 
                                width_option->value == 0 ? *default_width_option : *width_option, *spacing_option, 
                                max_nozzle_diameter, layer_height_option->value, 
                                std::min(overlap_ratio, float(solid_infill_overlap_option->get_effective_value(1.))), 0);
                            if (flow.width() < flow.height()) flow = flow.with_height(flow.width());
                            spacing_option->value = (width_option->percent) ? std::round(100 * flow.spacing() / max_nozzle_diameter) : (std::round(flow.spacing() * 10000) / 10000);
                        }
                        spacing_option->percent = width_option->percent;
                        something_changed = true;
                    }
                }
                if (opt_key == "top_infill_extrusion_width") {
                    const ConfigOptionPercent* top_solid_infill_overlap_option = find_option<ConfigOptionPercent>("top_solid_infill_overlap", this, config_collection);
                    spacing_option = this->option<ConfigOptionFloatOrPercent>("top_infill_extrusion_spacing");
                    if (width_option) {
                        width_option->set_phony(false);
                        spacing_option->set_phony(true);
                        if (width_option->value == 0)
                            spacing_option->value = 0;
                        else {
                            Flow flow = Flow::new_from_config_width(FlowRole::frTopSolidInfill, 
                                width_option->value == 0 ? *default_width_option : *width_option, *spacing_option, 
                                max_nozzle_diameter, layer_height_option->value,
                                std::min(overlap_ratio, float(top_solid_infill_overlap_option->get_effective_value(1.))), 0);
                            if (flow.width() < flow.height()) flow = flow.with_height(flow.width());
                            spacing_option->value = (width_option->percent) ? std::round(100 * flow.spacing() / max_nozzle_diameter) : (std::round(flow.spacing() * 10000) / 10000);
                        }
                        spacing_option->percent = width_option->percent;
                        something_changed = true;
                    }
                }
                //if (opt_key == "support_material_extrusion_width") {
                //    Flow flow = Flow::new_from_config_width(FlowRole::frSupportMaterial, width_option->value == 0 ? *default_width_option : *width_option, max_nozzle_diameter, layer_height_option->value, 0);
                //    if (width_option->percent)
                //        this->set_key_value("support_material_extrusion_spacing", new ConfigOptionFloatOrPercent(std::round(100 * flow.spacing() / max_nozzle_diameter), true));
                //    else
                //        this->set_key_value("support_material_extrusion_spacing", new ConfigOptionFloatOrPercent(std::round(flow.spacing() * 10000) / 10000, false));
                //    something_changed = true;
                //}
                //if (opt_key == "skirt_extrusion_width") {
                //    Flow flow = Flow::new_from_config_width(FlowRole::frPerimeter, width_option->value == 0 ? *default_width_option : *width_option, max_nozzle_diameter, layer_height_option->value, 0);
                //    if (width_option->percent)
                //        this->set_key_value("skirt_extrusion_spacing", new ConfigOptionFloatOrPercent(std::round(100 * flow.spacing() / max_nozzle_diameter), true));
                //    else
                //        this->set_key_value("skirt_extrusion_spacing", new ConfigOptionFloatOrPercent(std::round(flow.spacing() * 10000) / 10000, false));
                //    something_changed = true;
                //}
            } catch (FlowErrorNegativeSpacing) {
                if (spacing_option != nullptr) {
                    width_option->set_phony(true);
                    spacing_option->set_phony(false);
                    spacing_option->value = 100;
                    spacing_option->percent = true;
                    Flow flow = Flow::new_from_spacing(spacing_option->get_effective_value(max_nozzle_diameter), max_nozzle_diameter, layer_height_option->value, overlap_ratio, false);
                    width_option->value = (spacing_option->percent) ? std::round(100 * flow.width() / max_nozzle_diameter) : (std::round(flow.width() * 10000) / 10000);
                    width_option->percent = spacing_option->percent;
                    something_changed = true;
                } else {
                    width_option->value = 100;
                    width_option->percent = true;
                    width_option->set_phony(false);
                    spacing_option->set_phony(true);
                    Flow flow = Flow::new_from_config_width(FlowRole::frPerimeter, width_option->value == 0 ? *width_option : *default_width_option, *spacing_option, max_nozzle_diameter, layer_height_option->value, overlap_ratio, 0);
                    spacing_option->value = (width_option->percent) ? std::round(100 * flow.spacing() / max_nozzle_diameter) : (std::round(flow.spacing() * 10000) / 10000);
                    spacing_option->percent = width_option->percent;
                    something_changed = true;
                }
            }
        }
    }
    //update phony counterpark of 0-set fields
    // now they show 0, no need to update them
    //if (opt_key == "extrusion_width" || opt_key == "extrusion_spacing") {
    //    for (auto conf : config_collection) {
    //        if (conf->option("extrusion_width"))
    //            if (!conf->update_phony(config_collection, true).empty())
    //                return { conf };
    //    }
    //    return {};
    //}
    if(something_changed)
        return this;
    return nullptr;
}

CLIActionsConfigDef::CLIActionsConfigDef()
{
    ConfigOptionDef* def;

    // Actions:
    def = this->add("export_obj", coBool);
    def->label = L("Export OBJ");
    def->tooltip = L("Export the model(s) as OBJ.");
    def->set_default_value(new ConfigOptionBool(false));

/*
    def = this->add("export_svg", coBool);
    def->label = L("Export SVG");
    def->tooltip = L("Slice the model and export solid slices as SVG.");
    def->set_default_value(new ConfigOptionBool(false));
*/

    def = this->add("export_sla", coBool);
    def->label = L("Export SLA");
    def->tooltip = L("Slice the model and export SLA printing layers as PNG.");
    def->cli = "export-sla|sla";
    def->set_default_value(new ConfigOptionBool(false));

    def = this->add("export_3mf", coBool);
    def->label = L("Export 3MF");
    def->tooltip = L("Export the model(s) as 3MF.");
    def->set_default_value(new ConfigOptionBool(false));

    def = this->add("export_amf", coBool);
    def->label = L("Export AMF");
    def->tooltip = L("Export the model(s) as AMF.");
    def->set_default_value(new ConfigOptionBool(false));

    def = this->add("export_stl", coBool);
    def->label = L("Export STL");
    def->tooltip = L("Export the model(s) as STL.");
    def->set_default_value(new ConfigOptionBool(false));

    def = this->add("export_gcode", coBool);
    def->label = L("Export G-code");
    def->tooltip = L("Slice the model and export toolpaths as G-code.");
    def->cli = "export-gcode|gcode|g";
    def->set_default_value(new ConfigOptionBool(false));

    def = this->add("gcodeviewer", coBool);
    def->label = L("G-code viewer");
    def->tooltip = L("Visualize an already sliced and saved G-code");
    def->cli = "gcodeviewer";
    def->set_default_value(new ConfigOptionBool(false));

#if ENABLE_GL_CORE_PROFILE
    def = this->add("opengl-version", coString);
    def->label = L("OpenGL version");
    def->tooltip = L("Select a specific version of OpenGL");
    def->cli = "opengl-version";
    def->set_default_value(new ConfigOptionString());

    def = this->add("opengl-compatibility", coBool);
    def->label = L("OpenGL compatibility profile");
    def->tooltip = L("Enable OpenGL compatibility profile");
    def->cli = "opengl-compatibility";
    def->set_default_value(new ConfigOptionBool(false));

    def = this->add("opengl-debug", coBool);
    def->label = L("OpenGL debug output");
    def->tooltip = L("Activate OpenGL debug output on graphic cards which support it (OpenGL 4.3 or higher)");
    def->cli = "opengl-debug";
    def->set_default_value(new ConfigOptionBool(false));
#endif // ENABLE_GL_CORE_PROFILE

    def = this->add("slice", coBool);
    def->label = L("Slice");
    def->tooltip = L("Slice the model as FFF or SLA based on the printer_technology configuration value.");
    def->cli = "slice|s";
    def->set_default_value(new ConfigOptionBool(false));

    def = this->add("help", coBool);
    def->label = L("Help");
    def->tooltip = L("Show this help.");
    def->cli = "help|h";
    def->set_default_value(new ConfigOptionBool(false));

    def = this->add("help_fff", coBool);
    def->label = L("Help (FFF options)");
    def->tooltip = L("Show the full list of print/G-code configuration options.");
    def->set_default_value(new ConfigOptionBool(false));

    def = this->add("help_sla", coBool);
    def->label = L("Help (SLA options)");
    def->tooltip = L("Show the full list of SLA print configuration options.");
    def->set_default_value(new ConfigOptionBool(false));

    def = this->add("info", coBool);
    def->label = L("Output Model Info");
    def->tooltip = L("Write information about the model to the console.");
    def->set_default_value(new ConfigOptionBool(false));

    def = this->add("save", coString);
    def->label = L("Save config file");
    def->tooltip = L("Save configuration to the specified file.");
    def->set_default_value(new ConfigOptionString());
}

CLITransformConfigDef::CLITransformConfigDef()
{
    ConfigOptionDef* def;

    // Transform options:
    def = this->add("align_xy", coPoint);
    def->label = L("Align XY");
    def->tooltip = L("Align the model to the given point.");
    def->set_default_value(new ConfigOptionPoint(Vec2d(100,100)));

    def = this->add("cut", coFloat);
    def->label = L("Cut");
    def->tooltip = L("Cut model at the given Z.");
    def->set_default_value(new ConfigOptionFloat(0));

/*
    def = this->add("cut_grid", coFloat);
    def->label = L("Cut");
    def->tooltip = L("Cut model in the XY plane into tiles of the specified max size.");
    def->set_default_value(new ConfigOptionPoint());

    def = this->add("cut_x", coFloat);
    def->label = L("Cut");
    def->tooltip = L("Cut model at the given X.");
    def->set_default_value(new ConfigOptionFloat(0));

    def = this->add("cut_y", coFloat);
    def->label = L("Cut");
    def->tooltip = L("Cut model at the given Y.");
    def->set_default_value(new ConfigOptionFloat(0));
*/

    def = this->add("center", coPoint);
    def->label = L("Center");
    def->tooltip = L("Center the print around the given center.");
    def->set_default_value(new ConfigOptionPoint(Vec2d(100,100)));

    def = this->add("dont_arrange", coBool);
    def->label = L("Don't arrange");
    def->tooltip = L("Do not rearrange the given models before merging and keep their original XY coordinates.");

    def = this->add("ensure_on_bed", coBool);
    def->label = L("Ensure on bed");
    def->tooltip = L("Lift the object above the bed when it is partially below. Enabled by default, use --no-ensure-on-bed to disable.");
    def->set_default_value(new ConfigOptionBool(true));

    def = this->add("duplicate", coInt);
    def->label = L("Duplicate");
    def->tooltip =L("Multiply copies by this factor.");
    def->min = 1;

    def = this->add("duplicate_grid", coPoint);
    def->label = L("Duplicate by grid");
    def->tooltip = L("Multiply copies by creating a grid.");

    def = this->add("merge", coBool);
    def->label = L("Merge");
    def->tooltip = L("Arrange the supplied models in a plate and merge them in a single model in order to perform actions once.");
    def->cli = "merge|m";

    def = this->add("repair", coBool);
    def->label = L("Repair");
    def->tooltip = L("Try to repair any non-manifold meshes (this option is implicitly added whenever we need to slice the model to perform the requested action).");

    def = this->add("rotate", coFloat);
    def->label = L("Rotate");
    def->tooltip = L("Rotation angle around the Z axis in degrees.");
    def->set_default_value(new ConfigOptionFloat(0));

    def = this->add("rotate_x", coFloat);
    def->label = L("Rotate around X");
    def->tooltip = L("Rotation angle around the X axis in degrees.");
    def->set_default_value(new ConfigOptionFloat(0));

    def = this->add("rotate_y", coFloat);
    def->label = L("Rotate around Y");
    def->tooltip = L("Rotation angle around the Y axis in degrees.");
    def->set_default_value(new ConfigOptionFloat(0));

    def = this->add("scale", coFloatOrPercent);
    def->label = L("Scale");
    def->tooltip = L("Scaling factor or percentage.");
    def->set_default_value(new ConfigOptionFloatOrPercent(1, false));

    def = this->add("split", coBool);
    def->label = L("Split");
    def->tooltip = L("Detect unconnected parts in the given model(s) and split them into separate objects.");

    def = this->add("scale_to_fit", coPoint3);
    def->label = L("Scale to Fit");
    def->tooltip = L("Scale to fit the given volume.");
    def->set_default_value(new ConfigOptionPoint3(Vec3d(0,0,0)));

    def = this->add("delete-after-load", coString);
    def->label = L("Delete files after loading");
    def->tooltip = L("Delete files after loading.");
}

CLIMiscConfigDef::CLIMiscConfigDef()
{
    ConfigOptionDef* def;

    def = this->add("ignore_nonexistent_config", coBool);
    def->label = L("Ignore non-existent config files");
    def->tooltip = L("Do not fail if a file supplied to --load does not exist.");

    def = this->add("config_compatibility", coEnum);
    def->label = L("Forward-compatibility rule when loading configurations from config files and project files (3MF, AMF).");
    def->tooltip = L("This version of Slic3r may not understand configurations produced by the newest Slic3r versions. "
                     "For example, newer Slic3r may extend the list of supported firmware flavors. One may decide to "
                     "bail out or to substitute an unknown value with a default silently or verbosely.");
    def->set_enum<ForwardCompatibilitySubstitutionRule>({
        { "disable",        L("Bail out on unknown configuration values") },
        { "enable",         L("Enable reading unknown configuration values by verbosely substituting them with defaults.") },
        { "enable_silent",  L("Enable reading unknown configuration values by silently substituting them with defaults.") }
    });
    def->set_default_value(new ConfigOptionEnum<ForwardCompatibilitySubstitutionRule>(ForwardCompatibilitySubstitutionRule::Enable));

    def = this->add("load", coStrings);
    def->label = L("Load config file");
    def->tooltip = L("Load configuration from the specified file. It can be used more than once to load options from multiple files.");

    def = this->add("output", coString);
    def->label = L("Output File");
    def->tooltip = L("The file where the output will be written (if not specified, it will be based on the input file).");
    def->cli = "output|o";

    def = this->add("single_instance", coBool);
    def->label = L("Single instance mode");
    def->tooltip = L("If enabled, the command line arguments are sent to an existing instance of GUI Slic3r, "
                     "or an existing Slic3r window is activated. "
                     "Overrides the \"single_instance\" configuration value from application preferences.");

    def = this->add("datadir", coString);
    def->label = L("Data directory");
    def->tooltip = L("Load and store settings at the given directory. This is useful for maintaining different profiles or including configurations from a network storage.");

    def = this->add("threads", coInt);
    def->label = L("Maximum number of threads");
    def->invalidates_step = psGCodeExport;
    def->tooltip = L("Sets the maximum number of threads the slicing process will use. If not defined, it will be decided automatically.");
    def->min = 1;

    def = this->add("random_seed", coInt);
    def->label = L("Random seed");
    def->tooltip = L("Sets the random seed used by command line slicing. This is useful for deterministic regression tests.");
    def->min = 0;

    def = this->add("loglevel", coInt);
    def->label = L("Logging level");
    def->tooltip = L("Sets logging sensitivity. 0:fatal, 1:error, 2:warning, 3:info, 4:debug, 5:trace\n"
                     "For example. loglevel=2 logs fatal, error and warning level messages.");
    def->min = 0;

#if (defined(_MSC_VER) || defined(__MINGW32__)) && defined(SLIC3R_GUI)
    def = this->add("sw_renderer", coBool);
    def->label = L("Render with a software renderer");
    def->tooltip = L("Render with a software renderer. The bundled MESA software renderer is loaded instead of the default OpenGL driver.");
    def->min = 0;
#endif /* _MSC_VER */
}

const CLIActionsConfigDef    cli_actions_config_def;
const CLITransformConfigDef  cli_transform_config_def;
const CLIMiscConfigDef       cli_misc_config_def;

DynamicPrintAndCLIConfig::PrintAndCLIConfigDef& DynamicPrintAndCLIConfig::s_def_mutable()
{
    static PrintAndCLIConfigDef def;
    return def;
}

const DynamicPrintAndCLIConfig::PrintAndCLIConfigDef& DynamicPrintAndCLIConfig::s_def()
{
    return s_def_mutable();
}

void DynamicPrintAndCLIConfig::initialize_cli_def()
{
    s_def_mutable().initialize_from_print_config();
}

bool DynamicPrintAndCLIConfig::is_cli_def_initialized()
{
    return s_def().initialized();
}

bool DynamicPrintAndCLIConfig::read_cli(int argc, const char* const argv[], t_config_option_keys* extra, t_config_option_keys* keys)
{
    assert(is_cli_def_initialized());
    return DynamicConfig::read_cli(argc, argv, extra, keys);
}

#ifdef _DEBUGINFO
void DynamicPrintAndCLIConfig::handle_legacy(t_config_option_key &opt_key, std::string &value) const
{
    assert(false); // please handle it in bunch
    if (cli_actions_config_def  .options.find(opt_key) == cli_actions_config_def  .options.end() &&
        cli_transform_config_def.options.find(opt_key) == cli_transform_config_def.options.end() &&
        cli_misc_config_def     .options.find(opt_key) == cli_misc_config_def     .options.end()) {
        PrintConfigDef::handle_legacy_pair(opt_key, value);
    }
}
#endif

// SlicingStatesConfigDefs

ReadOnlySlicingStatesConfigDef::ReadOnlySlicingStatesConfigDef()
{
    ConfigOptionDef* def;

    def = this->add("zhop", coFloat);
    def->label = L("Current z-hop");
    def->tooltip = L("Contains z-hop present at the beginning of the custom G-code block.");
}

ReadWriteSlicingStatesConfigDef::ReadWriteSlicingStatesConfigDef()
{
    ConfigOptionDef* def;

    def = this->add("position", coFloats);
    def->label = L("Position");
    def->tooltip = L("Position of the extruder at the beginning of the custom G-code block. If the custom G-code travels somewhere else, "
                     "it should write to this variable so PrusaSlicer knows where it travels from when it gets control back.");

    def = this->add("e_retracted", coFloats);
    def->label = L("Retraction");
    def->tooltip = L("Retraction state at the beginning of the custom G-code block. If the custom G-code moves the extruder axis, "
                     "it should write to this variable so PrusaSlicer deretracts correctly when it gets control back.");

    def = this->add("e_restart_extra", coFloats);
    def->label = L("Extra deretraction");
    def->tooltip = L("Currently planned extra extruder priming after deretraction.");

    def = this->add("e_position", coFloats);
    def->label = L("Absolute E position");
    def->tooltip = L("Current position of the extruder axis. Only used with absolute extruder addressing.");
}

OtherSlicingStatesConfigDef::OtherSlicingStatesConfigDef()
{
    ConfigOptionDef* def;

    def = this->add("current_extruder", coInt);
    def->label = L("Current extruder");
    def->tooltip = L("Zero-based index of currently used extruder.");

    def = this->add("current_object_idx", coInt);
    def->label = L("Current object index");
    def->tooltip = L("Specific for sequential printing. Zero-based index of currently printed object.");

    def = this->add("has_single_extruder_multi_material_priming", coBool);
    def->label = L("Has single extruder MM priming");
    def->tooltip = L("Are the extra multi-material priming regions used in this print?");

    def = this->add("has_wipe_tower", coBool);
    def->label = L("Has wipe tower");
    def->tooltip = L("Whether or not wipe tower is being generated in the print.");

    def = this->add("initial_extruder", coInt);
    def->label = L("Initial extruder");
    def->tooltip = L("Zero-based index of the first extruder used in the print. Same as initial_tool.");

    def = this->add("initial_filament_type", coString);
    // TRN: Meaning 'filament type of the initial filament'
    def->label = L("Initial filament type");
    def->tooltip = L("String containing filament type of the first used extruder.");

    def = this->add("initial_tool", coInt);
    def->label = L("Initial tool");
    def->tooltip = L("Zero-based index of the first extruder used in the print. Same as initial_extruder.");

    def = this->add("is_extruder_used", coBools);
    def->label = L("Is extruder used?");
    def->tooltip = L("Vector of booleans stating whether a given extruder is used in the print.");
}

PrintStatisticsConfigDef::PrintStatisticsConfigDef()
{
    ConfigOptionDef* def;

    def = this->add("extruded_volume", coFloats);
    def->label = L("Volume per extruder");
    def->tooltip = L("Total filament volume extruded per extruder during the entire print.");

    def = this->add("normal_print_time", coString);
    def->label = L("Print time (normal mode)");
    def->tooltip = L("Estimated print time when printed in normal mode (i.e. not in silent mode). Same as print_time.");
    
    def = this->add("num_printing_extruders", coInt);
    def->label = L("Number of printing extruders");
    def->tooltip = L("Number of extruders used during the print.");

    def = this->add("print_time", coString);
    def->label = L("Print time (normal mode)");
    def->tooltip = L("Estimated print time when printed in normal mode (i.e. not in silent mode). Same as normal_print_time.");

    def = this->add("printing_filament_types", coString);
    def->label = L("Used filament types");
    def->tooltip = L("Comma-separated list of all filament types used during the print.");

    def = this->add("silent_print_time", coString);
    def->label = L("Print time (silent mode)");
    def->tooltip = L("Estimated print time when printed in silent mode.");

    def = this->add("total_cost", coFloat);
    def->label = L("Total cost");
    def->tooltip = L("Total cost of all material used in the print. Calculated from cost in Filament Settings.");

    def = this->add("total_weight", coFloat);
    def->label = L("Total weight");
    def->tooltip = L("Total weight of the print. Calculated from density in Filament Settings.");

    def = this->add("total_wipe_tower_cost", coFloat);
    def->label = L("Total wipe tower cost");
    def->tooltip = L("Total cost of the material wasted on the wipe tower. Calculated from cost in Filament Settings.");

    def = this->add("total_wipe_tower_filament", coFloat);
    def->label = L("Wipe tower volume");
    def->tooltip = L("Total filament volume extruded on the wipe tower.");

    def = this->add("used_filament", coFloat);
    def->label = L("Used filament");
    def->tooltip = L("Total length of filament used in the print.");

    def = this->add("total_toolchanges", coInt);
    def->label = L("Total number of toolchanges");
    def->tooltip = L("Number of toolchanges during the print.");

    def = this->add("extruded_volume_total", coFloat);
    def->label = L("Total volume");
    def->tooltip = L("Total volume of filament used during the entire print.");

    def = this->add("extruded_weight", coFloats);
    def->label = L("Weight per extruder");
    def->tooltip = L("Weight per extruder extruded during the entire print. Calculated from density in Filament Settings.");

    def = this->add("extruded_weight_total", coFloat);
    def->label = L("Total weight");
    def->tooltip = L("Total weight of the print. Calculated from density in Filament Settings.");

    def = this->add("total_layer_count", coInt);
    def->label = L("Total layer count");
    def->tooltip = L("Number of layers in the entire print.");
}

ObjectsInfoConfigDef::ObjectsInfoConfigDef()
{
    ConfigOptionDef* def;

    def = this->add("num_objects", coInt);
    def->label = L("Number of objects");
    def->tooltip = L("Total number of objects in the print.");

    def = this->add("num_instances", coInt);
    def->label = L("Number of instances");
    def->tooltip = L("Total number of object instances in the print, summed over all objects.");

    def = this->add("scale", coStrings);
    def->label = L("Scale per object");
    def->tooltip = L("Contains a string with the information about what scaling was applied to the individual objects. "
                     "Indexing of the objects is zero-based (first object has index 0).\n"
                     "Example: 'x:100% y:50% z:100%'.");

    def = this->add("input_filename_base", coString);
    def->label = L("Input filename without extension");
    def->tooltip = L("Source filename of the first object, without extension.");
}

DimensionsConfigDef::DimensionsConfigDef()
{
    ConfigOptionDef* def;

    const std::string point_tooltip   = L("The vector has two elements: x and y coordinate of the point. Values in mm.");
    const std::string bb_size_tooltip = L("The vector has two elements: x and y dimension of the bounding box. Values in mm.");

    def = this->add("first_layer_print_convex_hull", coPoints);
    def->label = L("First layer convex hull");
    def->tooltip = L("Vector of points of the first layer convex hull. Each element has the following format: "
                     "'[x, y]' (x and y are floating-point numbers in mm).");

    def = this->add("first_layer_print_min", coFloats);
    def->label = L("Bottom-left corner of first layer bounding box");
    def->tooltip = point_tooltip;

    def = this->add("first_layer_print_max", coFloats);
    def->label = L("Top-right corner of first layer bounding box");
    def->tooltip = point_tooltip;

    def = this->add("first_layer_print_size", coFloats);
    def->label = L("Size of the first layer bounding box");
    def->tooltip = bb_size_tooltip;

    def = this->add("print_bed_min", coFloats);
    def->label = L("Bottom-left corner of print bed bounding box");
    def->tooltip = point_tooltip;

    def = this->add("print_bed_max", coFloats);
    def->label = L("Top-right corner of print bed bounding box");
    def->tooltip = point_tooltip;

    def = this->add("print_bed_size", coFloats);
    def->label = L("Size of the print bed bounding box");
    def->tooltip = bb_size_tooltip;
}

TimestampsConfigDef::TimestampsConfigDef()
{
    ConfigOptionDef* def;

    def = this->add("timestamp", coString);
    def->label = L("Timestamp");
    def->tooltip = L("String containing current time in yyyyMMdd-hhmmss format.");

    def = this->add("year", coInt);
    def->label = L("Year");

    def = this->add("month", coInt);
    def->label = L("Month");

    def = this->add("day", coInt);
    def->label = L("Day");

    def = this->add("hour", coInt);
    def->label = L("Hour");

    def = this->add("minute", coInt);
    def->label = L("Minute");

    def = this->add("second", coInt);
    def->label = L("Second");
}

OtherPresetsConfigDef::OtherPresetsConfigDef()
{
    ConfigOptionDef* def;

    def = this->add("num_extruders", coInt);
    def->label = L("Number of extruders");
    def->tooltip = L("Total number of extruders, regardless of whether they are used in the current print.");

    def = this->add("num_milling", coInt);
    def->label = L("Number of mills");
    def->tooltip = L("Total number of mills, regardless of whether they are used in the current print.");

    def = this->add("print_preset", coString);
    def->label = L("Print preset name");
    def->tooltip = L("Name of the print preset used for slicing.");

    def = this->add("filament_preset", coStrings);
    def->label = L("Filament preset name");
    def->tooltip = L("Names of the filament presets used for slicing. The variable is a vector "
                     "containing one name for each extruder.");
    def->is_vector_extruder = true;

    def = this->add("printer_preset", coString);
    def->label = L("Printer preset name");
    def->tooltip = L("Name of the printer preset used for slicing.");

    def = this->add("physical_printer_preset", coString);
    def->label = L("Physical printer name");
    def->tooltip = L("Name of the physical printer used for slicing.");
}


static std::map<t_custom_gcode_key, t_config_option_keys> s_CustomGcodeSpecificPlaceholders{
    {"start_filament_gcode",    {"layer_num", "layer_z", "max_layer_z", "filament_extruder_id", "previous_extruder", "next_extruder"}},
    {"end_filament_gcode",      {"layer_num", "layer_z", "max_layer_z", "filament_extruder_id", "previous_extruder", "next_extruder"}},
    {"milling_toolchange_start_gcode", {"layer_num", "layer_z", "previous_layer_z", "max_layer_z", "previous_extruder", "next_extruder"}},
    {"milling_toolchange_end_gcode",   {"layer_num", "layer_z", "previous_layer_z", "max_layer_z", "previous_extruder", "next_extruder"}},
    {"start_gcode",             {"start_gcode_bed_temperature"}},
    {"end_gcode",               {"layer_num", "layer_z", "max_layer_z", "filament_extruder_id", "previous_extruder", "next_extruder"}},
    {"before_layer_gcode",      {"layer_num", "layer_z", "previous_layer_z", "max_layer_z", "gcode_bed_temperature", "layer_used_filament"}},
    {"layer_gcode",             {"layer_num", "layer_z", "previous_layer_z", "max_layer_z", "gcode_bed_temperature"}},
    {"feature_gcode",           {"layer_num", "layer_z", "max_layer_z", "previous_extrusion_role", "next_extrusion_role", /*deprecated*/"extrusion_role", "last_extrusion_role" /*deprecated*/}},
    {"toolchange_gcode",        {"layer_num", "layer_z", "max_layer_z", "previous_extruder", "next_extruder", "toolchange_z"}},
    {"color_change_gcode",      {"color_change_extruder", "next_color", "next_colour"}},
    {"pause_print_gcode",       {"color_change_extruder", "next_color", "next_colour"}},
    {"between_objects_gcode",   {"layer_num", "layer_z", "previous_object_id", "previous_object_name", "next_object_id", "next_object_name"}},
};

const std::map<t_custom_gcode_key, t_config_option_keys>& custom_gcode_specific_placeholders()
{
    return s_CustomGcodeSpecificPlaceholders;
}

CustomGcodeSpecificConfigDef::CustomGcodeSpecificConfigDef()
{
    ConfigOptionDef* def;

    def = this->add("layer_num", coInt);
    def->label = L("Layer number");
    def->tooltip = L("Zero-based index of the current layer (i.e. first layer is number 0).");

    def = this->add("layer_z", coFloat);
    def->label = L("Layer Z");
    def->tooltip = L("Height of the current layer above the print bed, measured to the top of the layer.");

    def = this->add("previous_layer_z", coFloat);
    def->label = L("Previous Layer Z");
    def->tooltip = L("Height of the previous layer.");

    def = this->add("max_layer_z", coFloat);
    def->label = L("Maximal layer Z");
    def->tooltip = L("Height of the last layer above the print bed.");

    def = this->add("filament_extruder_id", coInt);
    def->label = L("Current extruder index");
    def->tooltip = L("Zero-based index of currently used extruder (i.e. first extruder has index 0)."
        "\nWith multiple extruders, an extra 'end_filament_gcode' is applied at the end of the print for each extruder. In this case, 'filament_extruder_id' will be the index of the extruder to 'finalize'.");

    def = this->add("previous_extruder", coInt);
    def->label = L("Previous extruder");
    def->tooltip = L("Index of the extruder that is being unloaded. The index is zero based (first extruder has index 0). -1 if there is no extruder before (like in the start gcode)."
        "\nWith multiple extruders, an extra 'end_filament_gcode' is applied at the end of the print for each extruder. In this case, 'previous_extruder' will be the index of the last extruder used.");

    def = this->add("next_extruder", coInt);
    def->label = L("Next extruder");
    def->tooltip = L("Index of the extruder that is being loaded. The index is zero based (first extruder has index 0). -1 if there is no extruder after (like in the end gcode)."
        "\nWith multiple extruders, an extra 'end_filament_gcode' is applied at the end of the print for each extruder. In this case, 'next_extruder' will be -1.");

    def = this->add("toolchange_z", coFloat);
    def->label = L("Toolchange Z");
    def->tooltip = L("Height above the print bed when the toolchange takes place. Usually the same as layer_z, but can be different.");

    def = this->add("color_change_extruder", coInt);
    // TRN: This is a label in custom g-code editor dialog, belonging to color_change_extruder. Denoted index of the extruder for which color change is performed.
    def->label = L("Color change extruder");
    def->tooltip = L("Index of the extruder for which color change will be performed. The index is zero based (first extruder has index 0).");
    
    def = this->add("next_color", coString);
    // TRN: This is a label in custom g-code editor dialog, belonging to color_change_extruder. Denoted index of the extruder for which color change is performed.
    def->label = L("Next color");
    def->tooltip = L("Next color to display when a color change is performed, in #ffffff format.");

    def = this->add("next_colour", coString);
    // TRN: This is a label in custom g-code editor dialog, belonging to color_change_extruder. Denoted index of the extruder for which color change is performed.
    def->label = L("Next colour");
    def->tooltip = L("Next color to display when a color change is performed, in #ffffff format (same as 'next_color', but for british people).");

    def = this->add("previous_extrusion_role", coString);
    def->label = L("Previous extrusion role");
    def->tooltip = L("The extrusion role before changing to the new one.");

    def = this->add("next_extrusion_role", coString);
    def->label = L("Next extrusion role");
    def->tooltip = L("The new extrusion role the gcode changes to.");

    def = this->add("extrusion_role", coString);
    def->label = L("Extrusion role");
    def->tooltip = L("Deprecated, use next_extrusion_role.");

    def = this->add("last_extrusion_role", coString);
    def->label = L("Last extrusion role");
    def->tooltip = L("Deprecated, use previous_extrusion_role.");
    
    def = this->add("start_gcode_bed_temperature", coInt);
    def->label = L("Computed bed temperature for first layer");
    def->tooltip = L("It's the 'print_first_layer_bed_temperature' if defined or the maximum of the 'first_layer_bed_temperature' used in the first layer.");

    def = this->add("gcode_bed_temperature", coInt);
    def->label = L("Computed bed temperature");
    def->tooltip = L("It's the 'print_bed_temperature' if defined or the maximum of the 'bed_temperature'.");

    def = this->add("layer_used_filament", coFloats);
    def->label = L("Computed used filaent for each extruder");
    def->tooltip = L("It's an array of mm of extruded filament at this layer, the layer that ends now. The first extruder is at index 0, and this array has the same "
                     "number of entries as the number of extruders as the printer.");

    def = this->add("previous_object_id", coInt);
    def->label = L("Index of the object that finished printing.");
    def->tooltip = L("0-based index, the index is the object's position in the right panel list in the platter tab, from top to bottom."
        "\nIt's the same id used for 'label object' gcode.");

    def = this->add("next_object_id", coInt);
    def->label = L("Index of the object that will start printing.");
    def->tooltip = L("0-based index, the index is the object's position in the right panel list in the platter tab, from top to bottom."
        "\nIt's the same id used for 'label object' gcode.");

    def = this->add("previous_object_name", coString);
    def->label = L("Name of the object that finished printing.");
    def->tooltip = L("It's the same name used for 'label object' gcode.");

    def = this->add("next_object_name", coString);
    def->label = L("Name of the object that will start printing.");
    def->tooltip = L("It's the same name used for 'label object' gcode.");

}

const CustomGcodeSpecificConfigDef custom_gcode_specific_config_def;

uint64_t ModelConfig::s_last_timestamp = 1;

static Points to_points(const std::vector<Vec2d> &dpts)
{
    Points pts; pts.reserve(dpts.size());
    for (auto &v : dpts)
        pts.emplace_back( scale_i(v.x()), scale_i(v.y()) );
    return pts;
}

Points get_bed_shape(const DynamicPrintConfig &config)
{
    const auto *bed_shape_opt = config.opt<ConfigOptionPoints>("bed_shape");
    if (!bed_shape_opt) {
        
        // Here, it is certain that the bed shape is missing, so an infinite one
        // has to be used, but still, the center of bed can be queried
        if (auto center_opt = config.opt<ConfigOptionPoint>("center"))
            return { scale_p(center_opt->value) };
        
        return {};
    }
    
    return to_points(bed_shape_opt->get_values());
}

static bool is_XL_printer(const std::string& printer_notes)
{
    return boost::algorithm::contains(printer_notes, "PRINTER_VENDOR_PRUSA3D")
        && boost::algorithm::contains(printer_notes, "PRINTER_MODEL_XL");
}

bool is_XL_printer(const DynamicPrintConfig &cfg)
{
    auto *printer_notes = cfg.opt<ConfigOptionString>("printer_notes");
    return printer_notes && is_XL_printer(printer_notes->value);
}

} // namespace Slic3r

#include <cereal/types/polymorphic.hpp>
CEREAL_REGISTER_TYPE(Slic3r::DynamicPrintConfig)
CEREAL_REGISTER_POLYMORPHIC_RELATION(Slic3r::DynamicConfig, Slic3r::DynamicPrintConfig)
