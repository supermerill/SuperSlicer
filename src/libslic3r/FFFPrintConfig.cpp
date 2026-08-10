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

#include "FFFPrintConfig.hpp"

#include <algorithm>
#include <cfloat>
#include <cmath>

#include <boost/algorithm/string/predicate.hpp>
#include <boost/nowide/iostream.hpp>

#include "Flow.hpp"
#include "PointUtils.hpp"
#include "PrintSteps.hpp"

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

#define CONFIG_OPTION_ENUM_DEFINE_STATIC_MAPS(NAME, ...) \
    static const t_config_enum_values& enum_keys_map_##NAME() { \
        static const t_config_enum_values keys_map __VA_ARGS__; \
        return keys_map; \
    } \
    template<> const t_config_enum_values& ConfigOptionEnum<NAME>::get_enum_values() { return enum_keys_map_##NAME(); } \
    template<> const t_config_enum_names& ConfigOptionEnum<NAME>::get_enum_names() { \
        static const t_config_enum_names keys_names = enum_names_from_keys_map(enum_keys_map_##NAME()); \
        return keys_names; \
    }

CONFIG_OPTION_ENUM_DEFINE_STATIC_MAPS(ArcFittingType, {
    { "disabled",       int(ArcFittingType::Disabled) },
    { "bambu",          int(ArcFittingType::Bambu) },
    { "emit_center",    int(ArcFittingType::ArcWelder) } // arcwelder
})
CONFIG_OPTION_ENUM_DEFINE_STATIC_MAPS(CompleteObjectSort, {
    {"nearest", cosNearest},
    {"object", cosObject},
    {"lowy", cosY},
    {"lowz", cosZ},
})
CONFIG_OPTION_ENUM_DEFINE_STATIC_MAPS(WipeAlgo, {
    {"linear", waLinear},
    {"quadra", waQuadra},
    {"expo", waHyper},
})
CONFIG_OPTION_ENUM_DEFINE_STATIC_MAPS(GCodeFlavor, {
    {"reprapfirmware",  gcfRepRap},
    {"repetier",        gcfRepetier},
    {"teacup",          gcfTeacup},
    {"makerware",       gcfMakerWare},
    {"marlin",          gcfMarlinLegacy },
    {"marlin2",         gcfMarlinFirmware },
    {"klipper",         gcfKlipper},
    {"sailfish",        gcfSailfish},
    {"smoothie",        gcfSmoothie},
    {"sprinter",        gcfSprinter},
    {"mach3",           gcfMach3},
    {"machinekit",      gcfMachinekit},
    {"no-extrusion",    gcfNoExtrusion},
})
CONFIG_OPTION_ENUM_DEFINE_STATIC_MAPS(MachineLimitsUsage, {
    {"emit_to_gcode",       int(MachineLimitsUsage::EmitToGCode)},
    {"time_estimate_only",  int(MachineLimitsUsage::TimeEstimateOnly)},
    {"limits",              int(MachineLimitsUsage::Limits)},
    {"ignore",              int(MachineLimitsUsage::Ignore)},
})
CONFIG_OPTION_ENUM_DEFINE_STATIC_MAPS(BridgeType, {
    {"nozzle",  uint8_t(BridgeType::btFromNozzle)},
    {"height",  uint8_t(BridgeType::btFromHeight)},
    {"flow",    uint8_t(BridgeType::btFromFlow)},
})
CONFIG_OPTION_ENUM_DEFINE_STATIC_MAPS(FuzzySkinType, {
    { "none",           int(FuzzySkinType::None) },
    { "external",       int(FuzzySkinType::External) },
    { "shell",          int(FuzzySkinType::Shell) },
    { "all",            int(FuzzySkinType::All) }
})
CONFIG_OPTION_ENUM_DEFINE_STATIC_MAPS(InfillPattern, {
    {"rectilinear",         ipRectilinear},
    {"alignedrectilinear",  ipAlignedRectilinear},
    {"monotonic",           ipMonotonic},
    {"grid",                ipGrid},
    {"triangles",           ipTriangles},
    {"stars",               ipStars},
    {"cubic",               ipCubic},
    {"line",                ipLine},
    {"monotoniclines",      ipMonotonicLines },
    {"concentric",          ipConcentric},
    {"honeycomb",           ipHoneycomb},
    {"3dhoneycomb",         ip3DHoneycomb},
    {"gyroid",              ipGyroid},
    {"hilbertcurve",        ipHilbertCurve},
    {"archimedeanchords",   ipArchimedeanChords},
    {"octagramspiral",      ipOctagramSpiral},
    {"smooth",              ipSmooth},
    {"smoothtriple",        ipSmoothTriple},
    {"smoothhilbert",       ipSmoothHilbert},
    {"rectiwithperimeter",  ipRectiWithPerimeter},
    {"scatteredrectilinear", ipScatteredRectilinear},
    {"sawtooth",            ipSawtooth},
    {"adaptivecubic",       ipAdaptiveCubic},
    {"supportcubic",        ipSupportCubic},
    {"lightning",           ipLightning},
    {"ensuring",            ipEnsuring},
    {"auto",                ipAuto}
})
CONFIG_OPTION_ENUM_DEFINE_STATIC_MAPS(IroningType, {
    { "top",            int(IroningType::TopSurfaces) },
    { "topmost",        int(IroningType::TopmostOnly) },
    { "solid",          int(IroningType::AllSolid) }
})
CONFIG_OPTION_ENUM_DEFINE_STATIC_MAPS(PerimeterDirection, {
    {"ccw_cw",  pdCCW_CW},
    {"ccw_ccw", pdCCW_CCW},
    {"cw_ccw",  pdCW_CCW},
    {"cw_cw",   pdCW_CW},
})
CONFIG_OPTION_ENUM_DEFINE_STATIC_MAPS(SupportMaterialPattern, {
    { "rectilinear",        smpRectilinear },
    { "rectilinear-grid",   smpRectilinearGrid },
    { "honeycomb",          smpHoneycomb }
})
CONFIG_OPTION_ENUM_DEFINE_STATIC_MAPS(SupportMaterialStyle, {
    { "grid",           smsGrid },
    { "snug",           smsSnug },
    { "tree",           smsTree },
    { "organic",        smsOrganic }
})
CONFIG_OPTION_ENUM_DEFINE_STATIC_MAPS(SeamPosition, {
        {"random",    spRandom},
        {"allrandom", spAllRandom},
        {"nearest",   spNearest}, // unused, replaced by cost
        {"cost",      spCost},
        {"aligned", spAligned},
        {"contiguous", spExtremlyAligned},
        {"rear", spRear},
        {"custom", spCustom}, // for seam object
})
CONFIG_OPTION_ENUM_DEFINE_STATIC_MAPS(SeamScarfType, {
    { "none",           int(SeamScarfType::None) },
    { "external",       int(SeamScarfType::External) },
    { "all",            int(SeamScarfType::All) },
})
CONFIG_OPTION_ENUM_DEFINE_STATIC_MAPS(DenseInfillAlgo, {
        { "automatic", dfaAutomatic },
        { "autonotfull", dfaAutoNotFull },
        { "autoenlarged", dfaAutoOrEnlarged },
        { "autosmall",  dfaAutoOrNothing},
        { "enlarged", dfaEnlarged },
})
CONFIG_OPTION_ENUM_DEFINE_STATIC_MAPS(NoPerimeterUnsupportedAlgo, {
        { "none", npuaNone },
        { "noperi", npuaNoPeri },
        { "bridges", npuaBridges },
        { "bridgesoverhangs", npuaBridgesOverhangs },
        { "filled", npuaFilled },
})
CONFIG_OPTION_ENUM_DEFINE_STATIC_MAPS(InfillConnection, {
        { "connected", icConnected },
        { "holes", icHoles },
        { "outershell", icOuterShell },
        { "notconnected", icNotConnected },
})
CONFIG_OPTION_ENUM_DEFINE_STATIC_MAPS(RemainingTimeType, {
    { "m117", rtM117 },
    { "m73", rtM73 },
    { "m73q", rtM73_Quiet },
    { "m73m117", rtM73_M117 },
    { "none", rtNone },
})
CONFIG_OPTION_ENUM_DEFINE_STATIC_MAPS(SupportZDistanceType, {
    { "filament", zdFilament },
    { "plane", zdPlane },
    { "none", zdNone },
})
CONFIG_OPTION_ENUM_DEFINE_STATIC_MAPS(BrimType, {
    {"no_brim",         btNoBrim},
    {"outer_only",      btOuterOnly},
    {"inner_only",      btInnerOnly},
    {"outer_and_inner", btOuterAndInner}
})
CONFIG_OPTION_ENUM_DEFINE_STATIC_MAPS(DraftShield, {
    { "disabled", dsDisabled },
    { "limited",  dsLimited  },
    { "enabled",  dsEnabled  }
})
CONFIG_OPTION_ENUM_DEFINE_STATIC_MAPS(LabelObjectsStyle, {
    { "disabled",  int(LabelObjectsStyle::Disabled)  },
    { "octoprint", int(LabelObjectsStyle::Octoprint) },
    { "firmware",  int(LabelObjectsStyle::Firmware)  },
    { "both",      int(LabelObjectsStyle::Both)},
})
CONFIG_OPTION_ENUM_DEFINE_STATIC_MAPS(GCodeThumbnailsFormat, {
    { "PNG", int(GCodeThumbnailsFormat::PNG) },
    { "JPG", int(GCodeThumbnailsFormat::JPG) },
    { "QOI", int(GCodeThumbnailsFormat::QOI) },
    { "BIQU",int(GCodeThumbnailsFormat::BIQU) },
})
CONFIG_OPTION_ENUM_DEFINE_STATIC_MAPS(PerimeterGeneratorType, {
    { "classic", int(PerimeterGeneratorType::Classic) },
    { "arachne", int(PerimeterGeneratorType::Arachne) }
})
CONFIG_OPTION_ENUM_DEFINE_STATIC_MAPS(EnsureVerticalShellThickness, {
    { "disabled", int(EnsureVerticalShellThickness::Disabled) },
    { "partial",  int(EnsureVerticalShellThickness::Partial)  },
    { "enabled",  int(EnsureVerticalShellThickness::Enabled)  },
    { "enabled_old",  int(EnsureVerticalShellThickness::Enabled_old)  },
})


/*
double min_object_distance(const ConfigBase &cfg)
{
    const ConfigOptionEnum<PrinterTechnology> *opt_printer_technology = cfg.option<ConfigOptionEnum<PrinterTechnology>>("printer_technology");
    auto printer_technology = opt_printer_technology ? opt_printer_technology->value : ptUnknown;
    double ret = 0.;

    if (printer_technology == ptSLA)
        ret = 6.;
    else {
        auto ecr_opt = cfg.option<ConfigOptionFloat>("extruder_clearance_radius");
        auto dd_opt  = cfg.option<ConfigOptionFloat>("duplicate_distance");
        auto co_opt  = cfg.option<ConfigOptionBool>("complete_objects");

        if (!ecr_opt || !dd_opt || !co_opt) ret = 0.;
        else {
            // min object distance is max(duplicate_distance, clearance_radius)
            ret = (co_opt->value && ecr_opt->value > dd_opt->value) ?
                      ecr_opt->value : dd_opt->value;
        }
    }

    return ret;
}*/

double min_object_distance(const PrintConfig& config)
{
    return min_object_distance(static_cast<const ConfigBase*>(&config));
}

double min_object_distance(const ConfigBase *config, double ref_height /* = 0*/)
{
    if (printer_technology(*config) == ptSLA) return 6.;

    const ConfigOptionFloat* dd_opt = config->option<ConfigOptionFloat>("duplicate_distance");
    //test if called from prusaslicer::l240 where it's called on an empty config...
    if (dd_opt == nullptr) return 0;

    double base_dist = 0;
    //std::cout << "START min_object_distance =>" << base_dist << "\n";
    const ConfigOptionBool* opt_complete_object = config->option<ConfigOptionBool>("complete_objects");
    const ConfigOption* opt_parallel_objects_step = config->option("parallel_objects_step");
    if ((opt_parallel_objects_step && opt_parallel_objects_step->get_float() > 0) || (opt_complete_object && opt_complete_object->value)) {
        double skirt_dist = 0;
        double brim_dist = 0;
        try {
            std::vector<double> vals = dynamic_cast<const ConfigOptionFloats*>(config->option("nozzle_diameter"))->get_values();
            double max_nozzle_diam = 0;
            for (double val : vals) max_nozzle_diam = std::fmax(max_nozzle_diam, val);

            // min object distance is max(duplicate_distance, clearance_radius)
            // add 1 as safety offset.
            const double extruder_clearance_radius = config->option("extruder_clearance_radius")->get_float();
            if (extruder_clearance_radius > base_dist) {
                base_dist = extruder_clearance_radius;
            }

            // Add also the skirt dist if per object, as the arrange & check method don't use it yet.
            // we use the max nozzle, just to be on the safe side
            //ideally, we should use print::first_layer_height()
            const double first_layer_height =
                dynamic_cast<const ConfigOptionFloatOrPercent *>(config->option("first_layer_height"))
                    ->get_effective_value(max_nozzle_diam);
            //add the skirt
            int skirts = config->option("skirts")->get_int();
            if (skirts > 0 && ref_height == 0)
                skirts += config->option("skirt_brim")->get_int();
            if (skirts > 0 && config->option("skirt_height")->get_int() >= 1 &&
                !config->option("complete_objects_one_skirt")->get_bool()) {
                float overlap_ratio = 1;
                //can't know the extruder, so we settle on the worst: 100%
                //if (config->option<ConfigOptionPercents>("filament_max_overlap")) overlap_ratio = config->get_computed_value("filament_max_overlap");
                if (ref_height == 0) {
                    skirt_dist = config->option("skirt_distance")->get_float();
                    Flow skirt_flow = Flow::new_from_config_width(
                        frPerimeter,
                        *Flow::extrusion_width_option("skirt", *config),
                        *Flow::extrusion_spacing_option("skirt", *config),
                        (float)max_nozzle_diam,
                        (float)first_layer_height,
                        overlap_ratio,
                        0
                    );
                    skirt_dist += skirt_flow.width() + (skirt_flow.spacing() * ((double)skirts - 1));
                } else {
                    double skirt_height = ((double)config->option("skirt_height")->get_int() - 1) * config->get_computed_value("layer_height") + first_layer_height;
                    if (ref_height <= skirt_height) {
                        skirt_dist = config->option("skirt_distance")->get_float();
                        Flow skirt_flow = Flow::new_from_config_width(
                            frPerimeter,
                            *Flow::extrusion_width_option("skirt", *config),
                            *Flow::extrusion_spacing_option("skirt", *config),
                            (float)max_nozzle_diam,
                            (float)first_layer_height,
                            overlap_ratio,
                            0
                        );
                        skirt_dist += skirt_flow.width() + (skirt_flow.spacing() * ((double)skirts - 1));
                    }
                }
                // send a warning in print.validate if oneskirt, the skirt height is > 1mm and the skirt distance (from brim) is < extruder_clearance_radius
                // send a warning in print.validate if not oneskirt and skirt height > 1mm (you might collide the skirt while printing another one)
            }
            // Add also the biggest object brim, as the arrange & check method don't use it yet.
            // mm we don't have access to each object config... then send a warning in print.validate.
            const ConfigOption *opt_brim_per_object = config->option("brim_per_object");
            const ConfigOption *opt_skirt_distance_from_brim = config->option("skirt_distance_from_brim");
            const bool has_brim = (ref_height == 0 && opt_brim_per_object && opt_brim_per_object->get_bool());
            const bool skirt_is_pushed = skirt_dist > 0 && opt_skirt_distance_from_brim && opt_skirt_distance_from_brim->get_bool();
            if ( has_brim || skirt_is_pushed) {
                double max_brim = config->option("brim_width")->get_float();
                max_brim = std::max(max_brim, config->option("brim_width_interior")->get_float());
            }

            // if skirt_distance_from_brim, then push it further back
            if (skirt_is_pushed) {
                skirt_dist += brim_dist;
                brim_dist = 0;
            }
        }
        catch (const std::exception & ex) {
            boost::nowide::cerr << ex.what() << std::endl;
        }

        return base_dist + std::max(skirt_dist, brim_dist);
    }
    // else (not complete object/step)
    return base_dist;
}

//FIXME localize this function.
//note: seems only called for config export & command line. Most of the validation work for the gui is done elsewhere... So this function may be a bit out-of-sync
std::string validate(const FullPrintConfig& cfg)
{
    // --layer-height
    if (cfg.get_computed_value("layer_height") <= 0)
        return "Invalid value for --layer-height";
    if (fabs(fmod(cfg.get_computed_value("layer_height"), SCALING_FACTOR)) > 1e-4)
        return "--layer-height must be a multiple of print resolution";

    // --first-layer-height
    //if (cfg.get_effective_value("first_layer_height") <= 0) //can't do that, as the extruder isn't defined
    if(cfg.first_layer_height.value <= 0)
        return "Invalid value for --first-layer-height";

    // --filament-diameter
    for (double fd : cfg.filament_diameter.get_values())
        if (fd < 1)
            return "Invalid value for --filament-diameter";

    // --nozzle-diameter
    for (double nd : cfg.nozzle_diameter.get_values())
        if (nd < 0.005)
            return "Invalid value for --nozzle-diameter";

    // --perimeters
    if (cfg.perimeters.value < 0)
        return "Invalid value for --perimeters";

    // --solid-layers
    if (cfg.top_solid_layers < 0)
        return "Invalid value for --top-solid-layers";
    if (cfg.bottom_solid_layers < 0)
        return "Invalid value for --bottom-solid-layers";

    if (cfg.use_firmware_retraction.value &&
        cfg.gcode_flavor.value != gcfSmoothie &&
        cfg.gcode_flavor.value != gcfSprinter &&
        cfg.gcode_flavor.value != gcfRepRap &&
        cfg.gcode_flavor.value != gcfMarlinLegacy &&
        cfg.gcode_flavor.value != gcfMarlinFirmware &&
        cfg.gcode_flavor.value != gcfMachinekit &&
        cfg.gcode_flavor.value != gcfRepetier &&
        cfg.gcode_flavor.value != gcfKlipper)
        return "--use-firmware-retraction is only supported by Marlin 1&2, Smoothie, Sprinter, Reprap, Repetier, Machinekit, Repetier, Klipper? and Lerdge firmware";

    if (cfg.use_firmware_retraction.value)
        for (unsigned char wipe : cfg.wipe.get_values())
             if (wipe)
                return "--use-firmware-retraction is not compatible with --wipe";

    // --gcode-flavor
    if (! PrintConfigDef::instance().get("gcode_flavor")->has_enum_value(cfg.gcode_flavor.serialize()))
        return "Invalid value for --gcode-flavor";

    // --fill-pattern
    if (! PrintConfigDef::instance().get("fill_pattern")->has_enum_value(cfg.fill_pattern.serialize()))
        return "Invalid value for --fill-pattern";

    // --top-fill-pattern
    if (!PrintConfigDef::instance().get("top_fill_pattern")->has_enum_value(cfg.top_fill_pattern.serialize()))
        return "Invalid value for --top-fill-pattern";

    // --bottom-fill-pattern
    if (! PrintConfigDef::instance().get("bottom_fill_pattern")->has_enum_value(cfg.bottom_fill_pattern.serialize()))
        return "Invalid value for --bottom-fill-pattern";

    // --solid-fill-pattern
    if (!PrintConfigDef::instance().get("solid_fill_pattern")->has_enum_value(cfg.solid_fill_pattern.serialize()))
        return "Invalid value for --solid-fill-pattern";

    // --fill-density
    if (fabs(cfg.fill_density.value - 100.) < EPSILON &&
        (! PrintConfigDef::instance().get("top_fill_pattern")->has_enum_value(cfg.fill_pattern.serialize())
        && ! PrintConfigDef::instance().get("bottom_fill_pattern")->has_enum_value(cfg.fill_pattern.serialize())
        ))
        return "The selected fill pattern is not supposed to work at 100% density";

    // --infill-every-layers
    if (cfg.infill_every_layers < 1)
        return "Invalid value for --infill-every-layers";

    // --skirt-height
    if (cfg.skirt_height < 0)
        return "Invalid value for --skirt-height";

    // extruder clearance
    if (cfg.extruder_clearance_radius < 0)
        return "Invalid value for --extruder-clearance-radius";
    if (cfg.extruder_clearance_height < 0)
        return "Invalid value for --extruder-clearance-height";

    // --extrusion-multiplier
    for (double em : cfg.extrusion_multiplier.get_values())
        if (em <= 0)
            return "Invalid value for --extrusion-multiplier";

    // --spiral-vase
    if (cfg.spiral_vase) {
        // Note that we might want to have more than one perimeter on the bottom
        // solid layers.
        if (cfg.perimeters > 1)
            return "Can't make more than one perimeter when spiral vase mode is enabled";
        else if (cfg.perimeters < 1)
            return "Can't make less than one perimeter when spiral vase mode is enabled";
        if (cfg.fill_density > 0)
            return "Spiral vase mode can only print hollow objects, so you need to set Fill density to 0";
        if (cfg.top_solid_layers > 0)
            return "Spiral vase mode is not compatible with top solid layers";
        if (cfg.support_material || cfg.support_material_enforce_layers > 0)
            return "Spiral vase mode is not compatible with support material";
        if (cfg.infill_dense)
            return "Spiral vase mode can only print hollow objects and have no top surface, so you don't need any dense infill";
        if (cfg.extra_perimeters || cfg.extra_perimeters_below_area.value > 0 || cfg.extra_perimeters_count.value > 0 || cfg.extra_perimeters_on_overhangs || cfg.extra_perimeters_odd_layers)
            return "Can't make more than one perimeter when spiral vase mode is enabled";
        if (cfg.overhangs_reverse)
            return "Can't reverse the direction of the overhangs every layer when spiral vase mode is enabled";
        if (cfg.perimeter_reverse)
            return "Can't reverse the direction of the perimeters every layer when spiral vase mode is enabled";
    }

    // extrusion widths
    {
        double max_nozzle_diameter = 0.;
        for (double dmr : cfg.nozzle_diameter.get_values())
            max_nozzle_diameter = std::max(max_nozzle_diameter, dmr);
        const char *widths[] = { "", "external_perimeter_", "perimeter_", "infill_", "solid_infill_", "top_infill_", "support_material_", "first_layer_", "first_layer_infill_", "skirt_" };
        for (size_t i = 0; i < sizeof(widths) / sizeof(widths[i]); ++ i) {
            std::string key(widths[i]);
            key += "extrusion_width";
            if (cfg.option(key)->get_effective_value(max_nozzle_diameter) > 10. * max_nozzle_diameter)
                return std::string("Invalid extrusion width (too large): ") + key;
        }
    }

    // Out of range validation of numeric values.
    for (const std::string &opt_key : cfg.keys()) {
        const ConfigOption      *opt    = cfg.optptr(opt_key);
        assert(opt != nullptr);
        const ConfigOptionDef   *optdef = PrintConfigDef::instance().get(opt_key);
        assert(optdef != nullptr);

        if (!opt->is_enabled()) {
            // Do not check disabled values
            continue;
        }

        bool out_of_range = false;
        switch (opt->type()) {
        case coFloat:
        case coPercent:
        {
            auto *fopt = static_cast<const ConfigOptionFloat*>(opt);
            out_of_range = fopt->value < optdef->min || fopt->value > optdef->max;
            break;
        }
        case coFloatOrPercent:
        {
            auto *fopt = static_cast<const ConfigOptionFloatOrPercent*>(opt);
            out_of_range = fopt->get_effective_value(1) < optdef->min || fopt->get_effective_value(1) > optdef->max;
            break;
        }
        case coPercents:
        case coFloats:
        {
            const auto* vec = static_cast<const ConfigOptionVector<double>*>(opt);
            for (size_t i = 0; i < vec->size(); ++i) {
                if (!vec->is_enabled(i))
                    continue;
                double v = vec->get_at(i);
                if (v < optdef->min || v > optdef->max) {
                    out_of_range = true;
                    break;
                }
            }
            break;
        }
        case coFloatsOrPercents:
        {
            const auto* vec = static_cast<const ConfigOptionVector<FloatOrPercent>*>(opt);
            for (size_t i = 0; i < vec->size(); ++i) {
                if (!vec->is_enabled(i))
                    continue;
                const FloatOrPercent &v = vec->get_at(i);
                if (v.value < optdef->min || v.value > optdef->max) {
                    out_of_range = true;
                    break;
                }
            }
            break;
        }
        case coInt:
        {
            auto *iopt = static_cast<const ConfigOptionInt*>(opt);
            out_of_range = iopt->value < optdef->min || iopt->value > optdef->max;
            break;
        }
        case coInts:
        {
            const auto* vec = static_cast<const ConfigOptionVector<int32_t>*>(opt);
            for (size_t i = 0; i < vec->size(); ++i) {
                if (!vec->is_enabled(i))
                    continue;
                int v = vec->get_at(i);
                if (v < optdef->min || v > optdef->max) {
                    out_of_range = true;
                    break;
                }
            }
            break;
        }
        default:;
        }
        if (out_of_range)
            return std::string("Value out of range: " + opt_key);
    }

    // The configuration is valid.
    return "";
}

// Declare and initialize static caches of StaticPrintConfig derived classes.
#define PRINT_CONFIG_CACHE_ELEMENT_DEFINITION(r, data, CLASS_NAME) StaticPrintConfig::StaticCache<class Slic3r::CLASS_NAME> BOOST_PP_CAT(CLASS_NAME::s_cache_, CLASS_NAME);
#define PRINT_CONFIG_CACHE_ELEMENT_INITIALIZATION(r, data, CLASS_NAME) Slic3r::CLASS_NAME::initialize_cache();
#define PRINT_CONFIG_CACHE_INITIALIZE(CLASSES_SEQ) \
    BOOST_PP_SEQ_FOR_EACH(PRINT_CONFIG_CACHE_ELEMENT_DEFINITION, _, BOOST_PP_TUPLE_TO_SEQ(CLASSES_SEQ)) \
    int fff_print_config_static_initializer() { \
        /* Putting a trace here to avoid the compiler to optimize out this function. */ \
        /*BOOST_LOG_TRIVIAL(trace) << "Initializing StaticPrintConfigs";*/ \
        /* Tamas: alternative solution through a static volatile int. Boost log pollutes stdout and prevents tests from generating clean output */ \
        static volatile int ret = 1; \
        BOOST_PP_SEQ_FOR_EACH(PRINT_CONFIG_CACHE_ELEMENT_INITIALIZATION, _, BOOST_PP_TUPLE_TO_SEQ(CLASSES_SEQ)) \
        return ret; \
    }
PRINT_CONFIG_CACHE_INITIALIZE((
    PrintObjectConfig, PrintRegionConfig, MachineEnvelopeConfig, GCodeConfig, PrintConfig, FullPrintConfig))

void initialize_fff_print_config_cache()
{
    static const int initialized = fff_print_config_static_initializer();
    (void)initialized;
}

static Points to_points(const std::vector<Vec2d> &dpts)
{
    Points pts;
    pts.reserve(dpts.size());
    for (const Vec2d &v : dpts)
        pts.emplace_back(scale_i(v.x()), scale_i(v.y()));
    return pts;
}

Points get_bed_shape(const PrintConfig &cfg)
{
    return to_points(cfg.bed_shape.get_values());
}

static bool is_XL_printer_notes(const std::string &printer_notes)
{
    return boost::algorithm::contains(printer_notes, "PRINTER_VENDOR_PRUSA3D")
        && boost::algorithm::contains(printer_notes, "PRINTER_MODEL_XL");
}

bool is_XL_printer(const PrintConfig &cfg)
{
    return is_XL_printer_notes(cfg.printer_notes.value);
}

namespace {
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

}

void init_categories(PrintConfigDef &definition)
{

    definition.option_keys(RAW_PRESET_TYPE_FFF_TOOL_EXTRUDER_RETRACTION).insert({
        "deretract_speed",
        "retract_before_travel",
        "retract_before_wipe",
        "retract_layer_change",
        "retract_length",
        "retract_length_toolchange",
        "retract_lift",
        "retract_lift_above",
        "retract_lift_before_travel",
        "retract_lift_below",
        "retract_lift_first_layer",
        "retract_lift_top",
        "retract_restart_extra",
        "retract_restart_extra_toolchange",
        "retract_restart_toolchange_on_perimeter",
        "retract_restart_wipe_toolchange",
        "retract_speed",
        "seam_gap",
        "seam_gap_external",
        "travel_lift_before_obstacle",
        // "travel_max_lift",
        "travel_ramping_lift",
        "travel_slope",
        "wipe",
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
    });

    // TODO: remove keys already in RAW_PRESET_TYPE_FFF_TOOL_EXTRUDER_RETRACTION
    definition.option_keys(RAW_PRESET_TYPE_FFF_TOOL_EXTRUDER).insert( {
        "default_filament_profile",
        "deretract_speed",
        "extruder_clearance",
        "extruder_colour",
        "extruder_extrusion_multiplier_speed",
        "extruder_fan_offset",
        "extruder_offset",
        "extruder_temperature_offset",
        "max_layer_height",
        "min_layer_height",
        "nozzle_diameter",
        "retract_before_travel",
        "retract_before_wipe",
        "retract_layer_change",
        "retract_length",
        "retract_length_toolchange",
        "retract_lift",
        "retract_lift_above",
        "retract_lift_before_travel",
        "retract_lift_below",
        "retract_lift_first_layer",
        "retract_lift_top",
        "retract_restart_extra",
        "retract_restart_extra_toolchange",
        "retract_restart_toolchange_on_perimeter",
        "retract_restart_wipe_toolchange",
        "retract_speed",
        "seam_gap",
        "seam_gap_external",
        "temperature_heat_speed",
        "tool_name",
        "travel_lift_before_obstacle",
        // "travel_max_lift",
        "travel_ramping_lift",
        "travel_slope",
        "wipe",
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
    });
    // extruder retraction are also extruder
    append(definition.option_keys(RAW_PRESET_TYPE_FFF_TOOL_EXTRUDER),
        definition.option_keys(RAW_PRESET_TYPE_FFF_TOOL_EXTRUDER_RETRACTION));

    definition.option_keys(RAW_PRESET_TYPE_FFF_PRINT).insert( {
        "print_version",
        "layer_height", 
        "first_layer_height",
        "perimeters",
        "perimeters_hole",
        "spiral_vase",
        "slice_closing_radius",
        "slice_merge_dent",
        "slice_merge_min_width",
        "slicing_mode",
        "top_solid_layers",
        "top_solid_min_thickness",
        "bottom_solid_layers",
        "bottom_solid_min_thickness",
        "solid_over_perimeters",
        "duplicate_distance",
        "ensure_vertical_shell_thickness",
        "extra_perimeters",
        "extra_perimeters_below_area",
        "extra_perimeters_count",
        "extra_perimeters_odd_layers",
        "extra_perimeters_on_overhangs",
        "avoid_crossing_curled_overhangs",
        "only_one_perimeter_first_layer",
        "only_one_perimeter_top",
        "only_one_perimeter_top_other_algo",
        "allow_empty_layers",
        "avoid_crossing_perimeters", 
        "avoid_crossing_not_first_layer",
        "avoid_crossing_top",
        "avoid_travel_island",
        "avoid_travel_island_weight",
        "thin_perimeters", "thin_perimeters_all",
        "overhangs",
        "overhangs_extrusion_spacing",
        "overhangs_type",
        "overhangs_speed",
        "overhangs_speed_enforce",
        "overhangs_flow_ratio",
        "overhangs_width",
        "overhangs_width_speed", 
        "overhangs_reverse",
        "overhangs_reverse_threshold",
        "perimeter_reverse",
        "perimeter_direction",
        "seam_position",
        "seam_angle_cost",
        "seam_notch_all",
        "seam_notch_angle",
        "seam_notch_inner",
        "seam_notch_outer",
        "seam_travel_cost",
        "seam_visibility",
        "staggered_inner_seams",
        // external_perimeters
        "external_perimeters_first",
        "external_perimeters_first_force",
        "seam_slope_type",
        "seam_slope_min_height",
        "seam_slope_max_length",
        "external_perimeters_nothole",
        "external_perimeters_hole",
        // fill pattern
        "fill_density",
        "fill_pattern",
        "fill_top_flow_ratio",
        "fill_smooth_width",
        "fill_smooth_distribution",
        "top_fill_pattern",
        "bottom_fill_pattern",
        "solid_fill_pattern",
        "bridge_fill_pattern",
        "infill_every_layers",
//      "infill_only_where_needed",
        "solid_infill_every_layers",
        "internal_bridge_min_width",
        // ironing
        "ironing",
        "ironing_type",
        "ironing_flowrate",
        "ironing_speed",
        "ironing_spacing",
        "ironing_angle",
        "fill_aligned_z",
        "fill_angle",
        "fill_angle_cross",
        "fill_angle_follow_model",
        "fill_angle_increment",
        "fill_angle_template",
        "bridge_angle",
        "solid_infill_below_area",
        "solid_infill_below_layer_area",
        "solid_infill_below_width",
        "only_retract_when_crossing_perimeters", "enforce_retract_first_layer",
        "infill_first",
        "avoid_crossing_perimeters_max_detour",
        "max_volumetric_extrusion_rate_slope_positive", "max_volumetric_extrusion_rate_slope_negative", 
        "min_width_top_surface",
        // speeds
        "default_speed",
        "bridge_speed",
        "internal_bridge_speed",
        "brim_speed",
        "external_perimeter_speed",
        "first_layer_speed",
        "first_layer_min_speed",
        "first_layer_speed_over_raft",
        "infill_speed",
        "overhangs_dynamic_flow",
        "overhangs_dynamic_speed",
        "perimeter_speed",
        "small_perimeter_speed",
        "small_perimeter_max_length",
        "small_perimeter_min_length",
        "solid_infill_speed",
        "support_material_interface_speed",
        "support_material_speed", 
        "support_material_xy_spacing",
        "top_solid_infill_speed",
        "travel_speed", "travel_speed_z",
        "max_print_speed",
        "autospeed_min_thin_flow",
        "max_volumetric_speed",
        // gapfill
        "gap_fill_enabled",
        "gap_fill_extension",
        "gap_fill_flow_match_perimeter",
        "gap_fill_last",
        "gap_fill_max_width",
        "gap_fill_min_area",
        "gap_fill_min_length",
        "gap_fill_min_width",
        "gap_fill_no_overhang",
        "gap_fill_overlap",
        "gap_fill_perimeter",
        "gap_fill_speed",
        // fuzzy
        "fuzzy_skin",
        "fuzzy_skin_point_dist",
        "fuzzy_skin_thickness",
        // acceleration
        "bridge_acceleration",
        "brim_acceleration",
        "default_acceleration",
        "external_perimeter_acceleration",
        "first_layer_acceleration",
        "first_layer_acceleration_over_raft",
        "gap_fill_acceleration",
        "infill_acceleration",
        "internal_bridge_acceleration",
        "ironing_acceleration",
        "overhangs_acceleration",
        "perimeter_acceleration",
        "solid_infill_acceleration",
        "support_material_acceleration",
        "support_material_interface_acceleration",
        "thin_walls_acceleration",
        "top_solid_infill_acceleration",
        "travel_acceleration",
        "travel_deceleration_use_target",
        // skirt
        "skirts",
        "skirt_distance",
        "skirt_distance_from_brim",
        "skirt_height",
        "skirt_brim",
        "skirt_extrusion_width",
        "min_skirt_length",
        "draft_shield",
        // brim
        "brim_per_object",
        "brim_width",
        "brim_width_interior",
        "brim_separation",
        //"brim_type",
        // support
        "support_material", "support_material_auto", "support_material_threshold", "support_material_enforce_layers",
        "raft_contact_distance",
        "raft_contact_distance_type",
        "raft_expansion",
        "raft_first_layer_density", 
        "raft_first_layer_expansion",
        "raft_layers",
        "raft_layer_height", "raft_interface_layer_height",
        "support_material_layer_height", "support_material_interface_layer_height",
        "support_material_pattern", "support_material_with_sheath", "support_material_spacing",
        "support_material_closing_radius", "support_material_style",
        "support_material_synchronize_layers",
        "support_material_angle",
        "support_material_angle_height",
        "support_material_interface_layers", "support_material_bottom_interface_layers",
        "support_material_top_interface_pattern",
        "support_material_bottom_interface_expansion",
        "support_material_bottom_interface_pattern",
        "support_material_interface_angle",
        "support_material_interface_angle_increment",
        "support_material_interface_spacing",
        "support_material_interface_contact_loops",
        "support_material_contact_distance_type",
        "support_material_contact_distance_top",
        "support_material_contact_distance_bottom",
        "support_material_buildplate_only",
        "support_tree_angle",
        "support_tree_angle_slow",
        "support_tree_branch_diameter",
        "support_tree_branch_diameter_angle",
        "support_tree_branch_diameter_double_wall", 
        "support_tree_branch_distance",
        "support_tree_tip_diameter",
        "support_tree_top_rate",
        // miscellaneous
        "notes", 
        "print_custom_variables",
        "complete_objects",
        "parallel_islands",
        "parallel_objects_step",
        "parallel_objects_step_max_z",
        "complete_objects_one_skirt",
        "complete_objects_sort",
        "extruder_clearance_radius", 
        "extruder_clearance_height", "gcode_comments", "gcode_label_objects", "output_filename_format", "post_process", "perimeter_extruder",
        "gcode_substitutions",
        "infill_extruder", "solid_infill_extruder", "support_material_extruder", "support_material_interface_extruder", 
        "ooze_prevention", "standby_temperature_delta", "interface_shells",
        "object_gcode",
        "region_gcode",
        // width & spacing
        "extrusion_spacing", 
        "extrusion_width", 
        "first_layer_extrusion_spacing", 
        "first_layer_extrusion_width", 
        "first_layer_infill_extrusion_spacing", 
        "first_layer_infill_extrusion_width", 
        "perimeter_round_corners",
        "perimeter_extrusion_spacing",
        "perimeter_extrusion_width",
        "perimeter_extrusion_change_odd_layers",
        "external_perimeter_extrusion_spacing",
        "external_perimeter_extrusion_width",
        "external_perimeter_extrusion_change_odd_layers",
        "infill_extrusion_spacing",
        "infill_extrusion_width",
        "infill_extrusion_change_odd_layers",
        "solid_infill_extrusion_spacing",
        "solid_infill_extrusion_width",
        "solid_infill_extrusion_change_odd_layers",
        "top_infill_extrusion_spacing",
        "top_infill_extrusion_width",
        "support_material_extrusion_width",
        // overlap, ratios
        "infill_overlap",
        "bridge_flow_ratio",
        "bridge_type",
        "solid_infill_overlap",
        "top_solid_infill_overlap",
        "infill_anchor",
        "infill_anchor_max",
        "over_bridge_flow_ratio",
        "bridge_overlap",
        "bridge_overlap_min",
        "first_layer_flow_ratio",
        "enforce_full_fill_volume",
        "external_infill_margin", "bridged_infill_margin",
        "internal_bridge_expansion",
        "small_area_infill_flow_compensation_model",
        "first_layer_strong_start",
        // compensation
        "first_layer_size_compensation",
        "first_layer_size_compensation_layers",
        "first_layer_size_compensation_no_collapse",
        "xy_size_compensation",
        "xy_inner_size_compensation",
        "hole_size_compensation",
        "hole_size_threshold",
//        "threads",
        // wipe tower
        "wipe_tower", "wipe_tower_x", "wipe_tower_y", "wipe_tower_width", "wipe_tower_rotation_angle",
        "wipe_tower_bridging",
        "wipe_tower_brim_width",
        "priming_position",
        "wipe_tower_cone_angle",
        "wipe_tower_extra_spacing",
        "wipe_tower_extruder",
        "wipe_tower_extrusion_width",
        "wipe_tower_rest_in_middle",
        "wipe_tower_no_sparse_layers",
        "wipe_tower_speed",
        "wipe_tower_wipe_starting_speed",
        "mmu_segmented_region_interlocking_depth", 
        "mmu_segmented_region_max_width",
        "single_extruder_multi_material_priming", 
        "compatible_printers", "compatible_printers_condition", "inherits", 
        "infill_dense", "infill_dense_algo",
        "no_perimeter_unsupported_algo",
        // "exact_last_layer_height",
        "perimeter_loop",
        "perimeter_loop_seam",
        "infill_connection", "infill_connection_solid", "infill_connection_top", "infill_connection_bottom", "infill_connection_bridge",
        "infill_filled_bottom", "infill_filled_solid", "infill_filled_top",
        "first_layer_infill_speed",
        // thin wall
        "thin_walls",
        "thin_walls_min_width",
        "thin_walls_overlap",
        "thin_walls_speed",
        "thin_walls_merge",
        //precision, smoothing
//        "arc_fitting", // in printer preset
        "model_precision",
        "resolution",
        "resolution_internal",
        "bridge_precision",
        "curve_smoothing_precision",
        "curve_smoothing_cutoff_dist",
        "curve_smoothing_angle_convex",
        "curve_smoothing_angle_concave",
        "print_extrusion_multiplier",
        "print_first_layer_temperature",
        "print_retract_length",
        "print_temperature",
        "print_bed_temperature",
        "print_first_layer_bed_temperature",
        "print_retract_lift",
        "external_perimeter_cut_corners",
        "external_perimeter_overlap",
        "perimeter_bonding",
        "perimeter_overlap",
        //milling
        "milling_after_z",
        "milling_post_process",
        "milling_extra_size",
        "milling_speed",
        //Arachne
        "perimeter_generator", "wall_transition_length", "wall_transition_filter_deviation", "wall_transition_angle",
        "wall_distribution_count", "min_feature_size", "min_bead_width",
    });

    // TODO: this is too special, it needs to be simplified & make more plugin-friendly
    definition.option_keys(RAW_PRESET_TYPE_FFF_FILAMENT_OVERRIDE).insert( {
        "deretract_speed",
        "retract_before_travel",
        "retract_before_wipe",
        "retract_layer_change",
        "retract_length",
        "retract_length_toolchange",
        "retract_lift",
        "retract_lift_above",
        "retract_lift_before_travel",
        "retract_lift_below",
        "retract_restart_extra",
        "retract_restart_extra_toolchange",
        "retract_restart_toolchange_on_perimeter",
        "retract_restart_wipe_toolchange",
        "retract_speed",
        "seam_gap",
        "temperature_heat_speed",
        "travel_lift_before_obstacle",
        // "travel_max_lift",
        "travel_ramping_lift",
        "travel_slope",
        "wipe",
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
    });

    definition.option_keys(RAW_PRESET_TYPE_FFF_FILAMENT).insert({
        "filament_colour", 
        "filament_custom_variables",
        "filament_diameter", "filament_type", "filament_soluble", "filament_notes",
        "filament_max_speed",
        "filament_max_volumetric_speed",
        "filament_max_wipe_tower_speed",
        "filament_multitool_ramming",
        "filament_multitool_ramming_volume",
        "filament_multitool_ramming_flow", 
        "filament_fill_top_flow_ratio",
        "filament_first_layer_flow_ratio",
        "extrusion_multiplier", "filament_density", "filament_cost", "filament_spool_weight", "filament_loading_speed", "filament_loading_speed_start", "filament_load_time",
        "filament_unloading_speed", "filament_toolchange_delay", "filament_unloading_speed_start", "filament_unload_time", "filament_cooling_moves",
        "filament_cooling_initial_speed", "filament_cooling_final_speed", "filament_ramming_parameters", "filament_minimal_purge_on_wipe_tower",
        "filament_max_overlap",
        "filament_shrink",
        "filament_use_skinnydip",  // skinnydip params start
        "filament_use_fast_skinnydip",
        "filament_skinnydip_distance",
        "filament_melt_zone_pause",
        "filament_cooling_zone_pause",
        "filament_toolchange_temp",
        "filament_enable_toolchange_temp",
        "filament_enable_toolchange_part_fan",
        "filament_toolchange_part_fan_speed",
        "filament_dip_insertion_speed",
        "filament_dip_extraction_speed",  //skinnydip params end
        "filament_bridge_pa", //pa
        "filament_bridge_internal_pa",
        "filament_brim_pa",
        "filament_pressure_advance",
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
        "filament_travel_pa", //pa end
        // Temperature
        "bed_temperature",
        "first_layer_bed_temperature",
        "first_layer_temperature",
        "idle_temperature", 
        "temperature",
        // "cooling",
        // "fan_always_on", (now default_fan_speed)
        // "min_fan_speed", (now fan_printer_min_speed)
        "default_fan_speed",
        "max_fan_speed",
        "bridge_fan_speed",
        "external_perimeter_fan_speed",
        "gap_fill_fan_speed",
        "infill_fan_speed",
        "internal_bridge_fan_speed",
        "overhangs_fan_speed",
        "overhangs_dynamic_fan_speed",
        "perimeter_fan_speed",
        "solid_infill_fan_speed",
        "support_material_fan_speed",
        "support_material_interface_fan_speed",
        "top_fan_speed",
        "disable_fan_first_layers",
        "full_fan_speed_layer",
        "fan_below_layer_time",
        "slowdown_below_layer_time",
        "max_speed_reduction",
        "min_print_speed",
        // custom gcode
        "start_filament_gcode", "end_filament_gcode",
        // Retract overrides
        "filament_retract_length", "filament_retract_lift", "filament_retract_lift_above", "filament_retract_lift_below", 
        "filament_retract_length_toolchange",
        "retract_restart_toolchange_on_perimeter",
        "retract_restart_wipe_toolchange",
        "filament_retract_speed", "filament_deretract_speed", "filament_retract_restart_extra", 
        "filament_retract_before_travel", "filament_retract_lift_before_travel",
        "filament_retract_layer_change", "filament_retract_before_wipe", 
        "filament_retract_restart_extra_toolchange",
        "filament_seam_gap",
        "filament_temperature_heat_speed",
        "filament_travel_lift_before_obstacle",
        // "filament_travel_max_lift",
        "filament_travel_ramping_lift",
        "filament_travel_slope",
        "filament_wipe",
        "filament_wipe_extra_perimeter",
        "filament_wipe_only_crossing", "filament_wipe_speed",
        "filament_wipe_return",
        "filament_wipe_inside_depth",
        "filament_wipe_inside_end",
        "filament_wipe_inside_start",
        "filament_wipe_lift",
        "filament_wipe_lift_length",
        "filament_wipe_min",
        // Profile compatibility
        "filament_vendor", "compatible_prints", "compatible_prints_condition", "compatible_printers", "compatible_printers_condition", "inherits",
        //merill adds
        "filament_wipe_advanced_pigment",
        "chamber_temperature",
        "filament_pressure_advance",
    });

    definition.option_keys(RAW_PRESET_TYPE_FFF_PRINTER_MACHINE_LIMITS).insert( {
        "machine_max_acceleration_extruding",
        "machine_max_acceleration_retracting",
        "machine_max_acceleration_travel",
        "machine_max_acceleration_x", "machine_max_acceleration_y", "machine_max_acceleration_z", "machine_max_acceleration_e",
        "machine_max_feedrate_x", "machine_max_feedrate_y", "machine_max_feedrate_z", "machine_max_feedrate_e",
        "machine_min_extruding_rate", "machine_min_travel_rate",
        "machine_max_jerk_x", "machine_max_jerk_y", "machine_max_jerk_z", "machine_max_jerk_e",
        "z_step"
    });

    definition.option_keys(RAW_PRESET_TYPE_FFF_PRINTER).insert( {
        "arc_fitting",
        "arc_fitting_ignore_holes",
        "arc_fitting_resolution",
        "arc_fitting_tolerance", //TODO: keep?
        "autoemit_temperature_commands",
        "printer_technology",
        "bed_shape", "bed_custom_texture", "bed_custom_model", "z_offset", "init_z_rotate",
        "binary_gcode",
        "fan_kickstart",
        "fan_speedup_overhangs",
        "fan_speedup_time",
        "fan_percentage",
        "fan_printer_min_speed",
        "gcode_ascii",
        "gcode_filename_illegal_char",
        "gcode_flavor",
        "gcode_precision_xyz",
        "gcode_precision_e",
        "use_relative_e_distances",
        "use_firmware_retraction", "use_volumetric_e", "variable_layer_height",
        "lift_min",
        "gcode_command_buffer",
        "gcode_min_length",
        "gcode_min_resolution",
        "max_gcode_per_second",
        //FIXME the print host keys are left here just for conversion from the Printer preset to Physical Printer preset.
        "host_type", "print_host", "printhost_apikey", "printhost_cafile", "printhost_port",
        "single_extruder_multi_material", 
        // custom gcode
        "start_gcode",
        "start_gcode_manual",
        "end_gcode",
        "before_layer_gcode",
        "layer_gcode",
        "toolchange_gcode",
        "color_change_gcode", "pause_print_gcode", "template_custom_gcode","feature_gcode",
        "between_objects_gcode",
        "between_objects_gcode_before_move",
        //printer fields
        "printer_custom_variables",
        "printer_vendor",
        "printer_model", 
        "printer_variant", 
        "printer_notes", 
         // mmu
         "cooling_tube_retraction",
         "cooling_tube_length", "high_current_on_filament_swap", "parking_pos_retraction", "extra_loading_move", "max_print_height", 
        "default_print_profile", "inherits",
        "remaining_times",
        "remaining_times_type",
        "silent_mode", 
        "machine_limits_usage",
        "thumbnails",
        "thumbnails_color",
        "thumbnails_custom_color",
        "thumbnails_end_file",
        "thumbnails_format",
        "thumbnails_tag_format",
        "thumbnails_with_bed",
        "wipe_advanced",
        "wipe_advanced_nozzle_melted_volume",
        "wipe_advanced_multiplier",
        "wipe_advanced_algo",
        "time_estimation_compensation",
        "time_cost",
        "time_start_gcode",
        "time_toolchange",
    });
    // machines limits are also pritner settings.
    append(definition.option_keys(RAW_PRESET_TYPE_FFF_PRINTER),
        definition.option_keys(RAW_PRESET_TYPE_FFF_PRINTER_MACHINE_LIMITS));
    // also add milling & extrusder into printer, for now.
    append(definition.option_keys(RAW_PRESET_TYPE_FFF_PRINTER),
        definition.option_keys(RAW_PRESET_TYPE_FFF_TOOL_MILLING));
    append(definition.option_keys(RAW_PRESET_TYPE_FFF_PRINTER),
        definition.option_keys(RAW_PRESET_TYPE_FFF_TOOL_EXTRUDER));

}


void init_milling_params(PrintConfigDef &definition)
{
    // ConfigOptionFloats, ConfigOptionPercents, ConfigOptionBools, ConfigOptionStrings
    definition.option_keys(RAW_PRESET_TYPE_FFF_TOOL_MILLING).insert( {
    "milling_diameter", "milling_toolchange_end_gcode", "milling_toolchange_start_gcode",
        //"milling_offset",
        //"milling_z_offset",
        "milling_z_lift",

    });

    ConfigOptionDef* def;

    // Milling Printer settings

    def = definition.add("milling_cutter", coInt, ptFFF);
    def->label = L("Milling cutter");
    def->category = OptionCategory::general;
    def->tooltip = L("The milling cutter to use (unless more specific extruder settings are specified). ");
    def->min = 0;  // 0 = inherit defaults
    def->set_enum_values(ConfigOptionDef::GUIType::i_enum_open, {
    //TRN Print Settings: "Bottom contact Z distance". Have to be as short as possible
        { "default",      L("Default") },
        { "1",    "1" },
        { "2",    "2" },
        { "3",    "3" },
        { "4",    "4" },
        { "5",    "5" },
        { "6",    "6" },
        { "7",    "7" },
        { "8",    "8" },
        { "9",    "9" },
    });

    def = definition.add("milling_diameter", coFloats, ptFFF);
    def->label = L("Milling diameter");
    def->category = OptionCategory::milling_extruders;
    def->invalidates_step = psGCodeExport;
    def->tooltip = L("This is the diameter of your cutting tool.");
    def->sidetext = L("mm");
    def->mode = comAdvancedE | comSuSi;
    def->is_vector_extruder = true;
    def->set_default_value(new ConfigOptionFloats(3.14));

    def = definition.add("milling_offset", coPoints, ptFFF);
    def->label = L("Tool offset");
    def->category = OptionCategory::extruders;
    def->tooltip = L("If your firmware doesn't handle the extruder displacement you need the G-code "
        "to take it into account. This option lets you specify the displacement of each extruder "
        "with respect to the first one. It expects positive coordinates (they will be subtracted "
        "from the XY coordinate).");
    def->sidetext = L("mm");
    def->mode = comAdvancedE | comSuSi;
    def->is_vector_extruder = true;
    def->set_default_value(new ConfigOptionPoints( Vec2d(0,0) ));

    def = definition.add("milling_z_offset", coFloats, ptFFF);
    def->label = L("Tool z offset");
    def->category = OptionCategory::extruders;
    def->tooltip = L(".");
    def->sidetext = L("mm");
    def->mode = comAdvancedE | comSuSi;
    def->is_vector_extruder = true;
    def->set_default_value(new ConfigOptionFloats(0));

    def = definition.add("milling_z_lift", coFloats, ptFFF);
    def->label = L("Tool z lift");
    def->category = OptionCategory::extruders;
    def->tooltip = L("Amount of lift for travel.");
    def->sidetext = L("mm");
    def->mode = comAdvancedE | comSuSi;
    def->set_default_value(new ConfigOptionFloats(2));

    def = definition.add("milling_toolchange_start_gcode", coStrings, ptFFF);
    def->label = L("G-Code to switch to this toolhead");
    def->category = OptionCategory::milling_extruders;
    def->invalidates_step = psGCodeExport;
    def->tooltip = L("Put here the gcode to change the toolhead (called after the g-code T{next_extruder}). You have access to {next_extruder} and {previous_extruder}."
        " next_extruder is the 'extruder number' of the new milling tool, it's equal to the index (beginning at 0) of the milling tool plus the number of extruders."
        " previous_extruder is the 'extruder number' of the previous tool, it may be a normal extruder, if it's below the number of extruders."
        " The number of extruder is available at {extruder} and the number of milling tool is available at {milling_cutter}.");
    def->multiline = true;
    def->full_width = true;
    def->height = 12;
    def->mode = comAdvancedE | comSuSi;
    def->is_vector_extruder = true;
    def->set_default_value(new ConfigOptionStrings(""));

    def = definition.add("milling_toolchange_end_gcode", coStrings, ptFFF);
    def->label = L("G-Code to switch from this toolhead");
    def->category = OptionCategory::milling_extruders;
    def->invalidates_step = psGCodeExport;
    def->tooltip = L("Enter here the gcode to end the toolhead action, like stopping the spindle. You have access to {next_extruder} and {previous_extruder}."
        " previous_extruder is the 'extruder number' of the current milling tool, it's equal to the index (beginning at 0) of the milling tool plus the number of extruders."
        " next_extruder is the 'extruder number' of the next tool, it may be a normal extruder, if it's below the number of extruders."
        " The number of extruder is available at {extruder}and the number of milling tool is available at {milling_cutter}.");
    def->multiline = true;
    def->full_width = true;
    def->height = 12;
    def->mode = comAdvancedE | comSuSi;
    def->is_vector_extruder = true;
    def->set_default_value(new ConfigOptionStrings(""));

    def = definition.add("milling_post_process", coBool, ptFFF);
    def->label = L("Milling post-processing");
    def->category = OptionCategory::milling;
    def->invalidates_step = psGCodeExport;
    def->tooltip = L("If activated, at the end of each layer, the printer will switch to a milling head and mill the external perimeters."
        "\nYou should set the 'Milling extra XY size' to a value high enough to have enough plastic to mill. Also, be sure that your piece is firmly glued to the bed.");
    def->mode = comAdvancedE | comSuSi;
    def->set_default_value(new ConfigOptionBool(false));

    def = definition.add("milling_extra_size", coFloatOrPercent, ptFFF);
    def->label = L("Milling extra XY size");
    def->category = OptionCategory::milling;
    def->invalidates_step = psGCodeExport;
    def->tooltip = L("This increases the size of the object by a certain amount to have enough plastic to mill."
        " You can set a number of mm or a percentage of the calculated optimal extra width (from flow calculation).");
    def->sidetext = L("mm or %");
    def->ratio_over = "computed_on_the_fly";
    def->max_literal = { 20, false };
    def->mode = comAdvancedE | comSuSi;
    def->set_default_value(new ConfigOptionFloatOrPercent(150, true));

    def = definition.add("milling_after_z", coFloatOrPercent, ptFFF);
    def->label = L("Milling only after");
    def->category = OptionCategory::milling;
    def->invalidates_step = psGCodeExport;
    def->tooltip = L("This setting restricts the post-process milling to a certain height, to avoid milling the bed. It can be a mm or a % of the first layer height (so it can depend on the object).");
    def->sidetext = L("mm or %");
    def->ratio_over = "first_layer_height";
    def->max_literal = { 10, false };
    def->mode = comAdvancedE | comSuSi;
    def->set_default_value(new ConfigOptionFloatOrPercent(200, true));

    def = definition.add("milling_speed", coFloat, ptFFF);
    def->label = L("Milling Speed");
    def->category = OptionCategory::milling;
    def->invalidates_step = psGCodeExport;
    def->tooltip = L("Speed for milling tool.");
    def->sidetext = L("mm/s");
    def->min = 0;
    def->mode = comAdvancedE | comSuSi;
    def->set_default_value(new ConfigOptionFloat(30));
}


void init_fff_params(PrintConfigDef &definition)
{
    init_milling_params(definition);
    init_categories(definition);

    // Maximum extruder temperature, bumped to 1500 to support printing of glass.
    const int max_temp = 1500;

    ConfigOptionDef* def;

    def = definition.add("allow_empty_layers", coBool, ptFFF);
    def->label = L("Allow empty layers");
    def->full_label = L("Allow empty layers");
    def->category = OptionCategory::slicing;
    def->invalidates_step = psGCodeExport;
    def->tooltip = L("Prevent the gcode builder from triggering an exception if a full layer is empty, and allow the print to start from thin air afterward.");
    def->mode = comExpert | comSuSi;
    def->set_default_value(new ConfigOptionBool(false));

    def = definition.add("arc_fitting", coEnum, ptFFF);
    def->label = L("Arc fitting");
    def->category = OptionCategory::firmware;
    def->invalidates_step = posSlice;
    def->tooltip = L("Enable to get a G-code file which has G2 and G3 moves. "
                     "G-code resolution will be used as the fitting tolerance.");
    def->set_enum<ArcFittingType>({
        { "disabled",       "Disabled" },
        { "emit_center",    "Enabled: G2/3 I J (ArcWelder)" },
        { "bambu",       "Enabled: G2/3 I J (Bambu)" },
    });
    def->mode = comAdvancedE | comPrusa;
    def->set_default_value(new ConfigOptionEnum<ArcFittingType>(ArcFittingType::Disabled));

    def = definition.add("arc_fitting_ignore_holes", coBool, ptFFF);
    def->label = L("Ignore holes");
    def->full_label = L("Arc fitting: ignore holes");
    def->category = OptionCategory::firmware;
    def->invalidates_step = posSlice;
    def->tooltip = L("Don't trnsform holes into arc (G2/G3). This applies to all perimeter holes.");
    def->mode = comAdvancedE | comSuSi;
    def->set_default_value(new ConfigOptionBool(false));

    def = definition.add("arc_fitting_resolution", coFloatOrPercent, ptFFF);
    def->label = L("Arc fitting resolution");
    def->sidetext = L("mm or %");
    def->category = OptionCategory::firmware;
    def->invalidates_step = posSlice;
    def->tooltip = L("When using the arc_fitting option, resolution used to simplify the path into an arc."
    "\n can be a mm or a % of the slice resolution.");
    def->mode = comExpert | comSuSi;
    def->min = 0;
    def->set_default_value(new ConfigOptionFloatOrPercent(100, true));

    def = definition.add("arc_fitting_tolerance", coFloatOrPercent, ptFFF);
    def->label = L("Arc fitting tolerance");
    def->sidetext = L("mm or %");
    def->category = OptionCategory::firmware;
    def->invalidates_step = posSlice;
    def->tooltip = L("When using the arc_fitting option, allow the curve to deviate a certain % from the collection of straight paths."
        "\nCan be a mm value or a percentage of the current extrusion width.");
    def->mode = comAdvancedE | comSuSi;
    def->min = 0;
    def->set_default_value(new ConfigOptionFloatOrPercent(5, true));

    def = definition.add("autospeed_min_thin_flow", coFloatOrPercent, ptFFF);
    def->label = L("Minimum flow for thin extrusions");
    def->category = OptionCategory::speed;
    def->invalidates_step = psGCodeExport;
    def->tooltip = L("Thin walls and gapfill have very thin extrusions (often at the ends)."
        " Using these thin extrusions force the autospeed to have a very low average speed, as the maximum speed is used for the thinnest extrusion."
        " This setting allow to have a threshold, enforcing the max speed to this flow."
        " Any lower flow will still use the max speed, creating a difference in mm3/s for these but keeping a high speed for all others."
        "\nTo keep the automatic computation for the Autospeed, but excluding the thin gap fill & thin wall from it, set this setting to 0."
        "\nCan be a % of the maximum flow (compute from print's autospeed max volumetric flow and filament's max volumetric flow.");
    def->sidetext = L("mm3/s or %");
    def->mode = comAdvancedE | comSuSi;
    def->min = 0;
    def->can_be_disabled = true;
    def->set_default_value(enable_default_option(new ConfigOptionFloatOrPercent(0, false)));

    def = definition.add("avoid_crossing_curled_overhangs", coBool, ptFFF);
    def->label = L("Avoid crossing curled overhangs (Experimental)");
    def->category = OptionCategory::perimeter;
    def->invalidates_step = posEstimateCurledExtrusions;
    // TRN PrintSettings: "Avoid crossing curled overhangs (Experimental)"
    def->tooltip = L("Plan travel moves such that the extruder avoids areas where the filament may be curled up. "
                   "This is mostly happening on steeper rounded overhangs and may cause a crash with the nozzle. "
                   "This feature slows down both the print and the G-code generation.");
    def->mode = comExpert | comPrusa;
    def->set_default_value(new ConfigOptionBool(false));

    def = definition.add("avoid_crossing_perimeters", coBool, ptFFF);
    def->label = L("Avoid crossing perimeters");
    def->category = OptionCategory::perimeter;
    def->invalidates_step = psGCodeExport;
    def->tooltip = L("Optimize travel moves in order to minimize the crossing of perimeters. "
        "This is mostly useful with Bowden extruders which suffer from oozing. "
        "This feature slows down both the print and the G-code generation.");
    def->mode = comAdvancedE | comPrusa;
    def->set_default_value(new ConfigOptionBool(false));

    def = definition.add("avoid_crossing_not_first_layer", coBool, ptFFF);
    def->label = L("Don't avoid crossing on 1st layer");
    def->full_label = L("Don't avoid crossing on 1st layer");
    def->category = OptionCategory::perimeter;
    def->invalidates_step = psGCodeExport;
    def->tooltip = L("Disable 'Avoid crossing perimeters' for the first layer.");
    def->mode = comAdvancedE | comSuSi;
    def->set_default_value(new ConfigOptionBool(true));

    def = definition.add("avoid_crossing_perimeters_max_detour", coFloatOrPercent, ptFFF);
    def->label = L("Avoid crossing perimeters - Max detour length");
    def->category = OptionCategory::perimeter;
    def->invalidates_step = psGCodeExport;
    def->tooltip = L("The maximum detour length for avoid crossing perimeters. "
                     "If the detour is longer than this value, avoid crossing perimeters is not applied for this travel path. "
                     "Detour length can be specified either as an absolute value or as percentage (for example 50%) of a direct travel path.");
    def->sidetext = L("mm or % (zero to disable)");
    def->min = 0;
    def->max_literal = { 1000, false };
    def->mode = comExpert | comPrusa;
    def->set_default_value(new ConfigOptionFloatOrPercent(0., false));

    def = definition.add("avoid_crossing_top", coBool, ptFFF);
    def->label = L("Avoid top surface for travels");
    def->category = OptionCategory::perimeter;
    def->invalidates_step = psGCodeExport;
    def->tooltip = L("When using 'Avoid crossing perimeters', consider the top surfaces as a void, to avoid travelling over them if possible.");
    def->mode = comAdvancedE | comSuSi;
    def->set_default_value(new ConfigOptionBool(true));

    def = definition.add("avoid_travel_island", coBool, ptFFF);
    def->label = L("Find smallest crossing between islands");
    def->category = OptionCategory::perimeter;
    def->invalidates_step = psGCodeExport;
    def->tooltip = L("When using 'Avoid crossing perimeters', if you need to travel between two islands, find the two points that are nearest to each other."
        "\nNote: In modifiers, only works in object & layer range modifiers.");
    def->mode = comAdvancedE | comSuSi;
    def->set_default_value(new ConfigOptionBool(true));

    def = definition.add("avoid_travel_island_weight", coFloat, ptFFF);
    def->label = L("Weight for internal travel");
    def->full_label = L("Weight for internal distance while choosing island crossing");
    def->category = OptionCategory::perimeter;
    def->invalidates_step = psGCodeExport;
    def->tooltip = L("When using 'Avoid crossing perimeters', and 'Find smallest travel between islands'"
        ", also consider the distance between the crossing points and the start & end of the travel while searching for the smallest crossing."
        "\nSet to zero to be sure to have the smallest crossing possible."
        "\nSet to a higher value to be able to choose a nearer crossing even if the crossing distance isn't as small as possible."
        "\nNote: In modifiers, only works in object & layer range modifiers.");
    def->min = 0;
    def->mode = comAdvancedE | comSuSi;
    def->set_default_value(new ConfigOptionFloat(0.4));

    def = definition.add("bed_temperature", coInts, ptFFF);
    def->label = L("Other layers");
    def->category = OptionCategory::filament;
    def->invalidates_step = psGCodeExport;
    def->tooltip = L("Bed temperature for layers after the first one. "
                   "Set zero to disable bed temperature control commands in the output.");
    def->sidetext = L("°C");
    def->full_label = L("Bed temperature");
    def->sidetext = L("°C");
    def->min = 0;
    def->max = 300;
    def->is_vector_extruder = true;
    def->mode = comSimpleAE | comPrusa;
    def->set_default_value(new ConfigOptionInts { 0 });

    def = definition.add("before_layer_gcode", coString, ptFFF);
    def->label = L("Before layer change G-code");
    def->category = OptionCategory::customgcode;
    def->invalidates_step = psGCodeExport;
    def->tooltip = L("This custom code is inserted at every layer change, right before the Z move. "
                   "Note that you can use placeholder variables for all Slic3r settings as well "
                   "as {layer_num} and {layer_z}.");
    def->multiline = true;
    def->full_width = true;
    def->height = 5;
    def->mode = comExpert | comPrusa;
    def->set_default_value(new ConfigOptionString(""));

    def = definition.add("between_objects_gcode", coString, ptFFF);
    def->label = L("Between objects G-code");
    def->category = OptionCategory::customgcode;
    def->invalidates_step = psGCodeExport;
    def->tooltip = L("This code is inserted between objects when using sequential printing. By default extruder and bed temperature are reset using non-wait command; however if M104, M109, M140 or M190 are detected in this custom code, Slic3r will not add temperature commands. Note that you can use placeholder variables for all Slic3r settings, so you can put a \"M109 S{first_layer_temperature}\" command wherever you want.");
    def->multiline = true;
    def->full_width = true;
    def->height = 12;
    def->mode = comExpert | comPrusa;
    def->set_default_value(new ConfigOptionString(""));

    def = definition.add("between_objects_gcode_before_move", coBool, ptFFF);
    def->label = L("Put gcode before the move");
    def->category = OptionCategory::customgcode;
    def->invalidates_step = psGCodeExport;
    def->tooltip = L("'This'between_objects_gcode' code is inserted before moving to the next object, instead of after moving to the next object.");
    def->mode = comExpert | comSuSi;
    def->set_default_value(new ConfigOptionBool(false));

    def = definition.add("bottom_solid_layers", coInt, ptFFF);
    //TRN Print Settings: "Bottom solid layers"
    def->label = L_CONTEXT("Bottom", "Layers");
    def->category = OptionCategory::perimeter;
    def->invalidates_step = posSlice;
    def->tooltip = L("Number of solid layers to generate on bottom surfaces.");
    def->full_label = L("Bottom solid layers");
    def->min = 0;
    def->mode = comSimpleAE | comPrusa;
    def->set_default_value(new ConfigOptionInt(3));

    def = definition.add("bottom_solid_min_thickness", coFloat, ptFFF);
    def->label = L_CONTEXT("Bottom", "Layers");
    def->category = OptionCategory::perimeter;
    def->invalidates_step = posPrepareInfill;
    def->tooltip = L("The number of bottom solid layers is increased above bottom_solid_layers if necessary to satisfy "
    				 "minimum thickness of bottom shell.");
    def->full_label = L("Minimum bottom shell thickness");
    def->sidetext = L("mm");
    def->min = 0;
    def->mode = comSimpleAE | comPrusa;
    def->set_default_value(new ConfigOptionFloat(0.));

    def = definition.add("bridge_acceleration", coFloatOrPercent, ptFFF);
    def->label = L("Bridges");
    def->full_label = L("Bridge acceleration");
    def->category = OptionCategory::speed;
    def->invalidates_step = psGCodeExport;
    def->tooltip = L("This is the acceleration your printer will use for bridges."
                "\nCan be a % of the default acceleration"
                "\nSet zero to use default acceleration for bridges.");
    def->sidetext = L("mm/s² or %");
    def->ratio_over = "default_acceleration";
    def->min = 0;
    def->max_literal = { -220, false };
    def->mode = comAdvancedE | comPrusa;
    def->set_default_value(new ConfigOptionFloatOrPercent(0,false));

    def = definition.add("bridge_angle", coFloat, ptFFF);
    def->label = L("Bridging");
    def->full_label = L("Bridging angle");
    def->category = OptionCategory::infill;
    def->invalidates_step = posPerimeters;
    def->tooltip = L("Bridging angle override."
                   "\nIf disabled, the bridging angle will be calculated automatically."
                   " Otherwise the provided angle will be used for all bridges."
                   "Note: 180° is the same as zero angle.");
    def->sidetext = L("°");
    def->min = 0;
    def->can_be_disabled = true;
    def->mode = comAdvancedE | comPrusa;
    def->set_default_value(disable_default_option(new ConfigOptionFloat(0.)));

    def = definition.add("bridged_infill_margin", coFloatOrPercent, ptFFF);
    def->label = L("Bridged");
    def->full_label = L("Bridge margin");
    def->category = OptionCategory::infill;
    def->invalidates_step = posPerimeters;
    def->tooltip = L("This parameter grows the bridged solid infill layers by the specified mm to anchor them into the sparse infill and over the perimeters below. Put 0 to deactivate it. Can be a % of the width of the external perimeter.");
    def->sidetext = L("mm or %");
    def->ratio_over = "external_perimeter_extrusion_width";
    def->min = 0;
    def->max_literal = { 50, true };
    def->mode = comExpert | comSuSi;
    def->set_default_value(new ConfigOptionFloatOrPercent(200, true));

    def = definition.add("bridge_fan_speed", coInts, ptFFF);
    def->label = L("Bridge Infill fan speed");
    def->category = OptionCategory::cooling;
    def->invalidates_step = psGCodeExport;
    def->tooltip = L("This fan speed is enforced during bridges and overhangs. It won't slow down the fan if it's currently running at a higher speed."
        "\nSet to 0 to stop the fan."
        "\nIf disabled, default fan speed will be used."
        "\nCan be disabled by disable_fan_first_layers and increased by low layer time.");
    def->sidetext = L("%");
    def->min = 0;
    def->max = 100;
    def->mode = comAdvancedE | comPrusa;
    def->is_vector_extruder = true;
    def->can_be_disabled = true;
    def->set_default_value(enable_default_option(new ConfigOptionInts{ 100 }));

    def = definition.add("bridge_fill_pattern", coEnum, ptFFF);
    def->label = L("Bridging fill pattern");
    def->category = OptionCategory::infill;
    def->invalidates_step = posInfill;
    def->tooltip = L("Fill pattern for bridges and internal bridge infill.");
    def->set_enum<InfillPattern>({
        { "rectilinear", L("Rectilinear") },
        { "monotonic", L("Monotonic") },
    });
    def->mode = comExpert | comSuSi;
    def->set_default_value(new ConfigOptionEnum<InfillPattern>(ipRectilinear));

    def = definition.add("bridge_type", coEnum, ptFFF);
    def->label = L("Bridge flow baseline");
    def->category = OptionCategory::width;
    def->invalidates_step = posSlice;
    def->tooltip = L("This setting allow you to choose the base for the bridge flow compute, the result will be multiplied by the bridge flow to have the final result."
        "\nA bridge is an infill extrusion with nothing under it to flatten it, and so it can't have a 'rectangle' shape but a circle one."
        "\n * The default way to compute a bridge flow is to use the nozzle diameter as the diameter of the extrusion cross-section. It shouldn't be higher than that to prevent sagging."
        "\n * A second way to compute a bridge flow is to use the current layer height, so it shouldn't protrude below it. Note that may create too thin extrusions and so a bad bridge quality."
        "\n * A Third way to compute a bridge flow is to continue to use the current flow/section (mm3 per mm). If there is no current flow, it will use the solid infill one."
        " To use if you have some difficulties with the big flow changes from infill flow to bridge flow and vice-versa, the bridge flow ratio let you compensate for the change in speed."
        " \nThe preview will display the expected shape of the bridge extrusion (cylinder), don't expect a magical thick and solid air to flatten the extrusion magically.");
    def->sidetext = L("%");
    def->set_enum<BridgeType>({
        { "nozzle", L("Nozzle diameter") },
        { "height", L("Layer height") },
        { "flow", L("Keep current flow") },
    });
    def->mode = comAdvancedE | comSuSi;
    def->set_default_value(new ConfigOptionEnum<BridgeType>{ BridgeType::btFromNozzle });

    def = definition.add("bridge_flow_ratio", coPercent, ptFFF);
    def->label = L("Bridge");
    def->full_label = L("Bridge flow ratio");
    def->sidetext = L("%");
    def->category = OptionCategory::width;
    def->invalidates_step = posSlice;
    def->tooltip = L("This factor affects the amount of plastic for bridging. "
                   "You can decrease it slightly to pull the extrudates and prevent sagging, "
                   "although default settings are usually good and you should experiment "
                   "with cooling (use a fan) before tweaking this."
                   "\nFor reference, the default bridge flow is (in mm3/mm): (nozzle diameter) * (nozzle diameter) * PI/4");
    def->min = 2;
    def->max = 1000;
    def->mode = comAdvancedE | comPrusa;
    def->set_default_value(new ConfigOptionPercent(100));

    def = definition.add("over_bridge_flow_ratio", coPercent, ptFFF);
    def->label = L("Above the bridges");
    def->full_label = L("Above bridge flow ratio");
    def->sidetext = L("%");
    def->category = OptionCategory::width;
    def->invalidates_step = posPrepareInfill;
    def->tooltip = L("Flow ratio to compensate for the gaps in a bridged top surface. Used for ironing infill"
        "pattern to prevent regions where the low-flow pass does not provide a smooth surface due to a lack of plastic."
        " You can increase it slightly to pull the top layer at the correct height. Recommended maximum: 120%.");
    def->min = 2;
    def->max = 1000;
    def->mode = comAdvancedE | comSuSi;
    def->set_default_value(new ConfigOptionPercent(100));

    def = definition.add("bridge_precision", coFloatOrPercent, ptFFF);
    def->label = L("Bridge precision");
    def->category = OptionCategory::slicing;
    def->invalidates_step = posSlice;
    def->tooltip = L("This is the precision of the bridge detection. If you put it too low, the bridge detection will be very inefficient."
                    "\nCan be a % of the bridge spacing.");
    def->sidetext = L("mm or %");
    def->min = 0;
    def->mode = comAdvancedE | comSuSi;
    def->set_default_value(new ConfigOptionFloatOrPercent(25, true));

    def = definition.add("bridge_overlap_min", coPercent, ptFFF);
    def->label = L("Min");
    def->full_label = L("Min bridge density");
    def->sidetext = L("%");
    def->category = OptionCategory::width;
    def->invalidates_step = posInfill;
    def->tooltip = L("Minimum density for bridge lines. If Lower than bridge_overlap, then the overlap value can be lowered automatically down to this value."
        " If the value is higher, this parameter has no effect."
        "\nDefault to 87.5% to allow a little void between the lines.");
    def->min = 2;
    def->max = 2000;
    def->mode = comExpert | comSuSi;
    def->set_default_value(new ConfigOptionPercent(80));

    def = definition.add("bridge_overlap", coPercent, ptFFF);
    def->label = L("Max");
    def->full_label = L("Max bridge density");
    def->sidetext = L("%");
    def->category = OptionCategory::width;
    def->invalidates_step = posInfill;
    def->tooltip = L("Maximum density for bridge lines. If you want more space between line (or less), you can modify it."
        " A value of 50% will create two times less lines, and a value of 200% will create two time more lines that overlap each other.");
    def->min = 2;
    def->max = 2000;
    def->mode = comExpert | comSuSi;
    def->set_default_value(new ConfigOptionPercent(90));

    def = definition.add("bridge_speed", coFloatOrPercent, ptFFF);
    def->label = L("Bridges");
    def->full_label = L("Bridge Infill speed");
    def->category = OptionCategory::speed;
    def->invalidates_step = psGCodeExport;
    def->tooltip = L("Speed for printing bridges."
        "\nThis can be expressed as a percentage (for example: 60%) over the Default speed."
        "\nSet zero to use the autospeed for this feature");
    def->sidetext = L("mm/s or %");
    def->aliases = { "bridge_feed_rate" };
    def->ratio_over = "default_speed";
    def->min = 0;
    def->mode = comAdvancedE | comPrusa;
    def->set_default_value(new ConfigOptionFloatOrPercent(60, true));

    def = definition.add("brim_per_object", coBool, ptFFF);
    def->label = L("Brim per object");
    def->category = OptionCategory::skirtBrim;
    def->invalidates_step = posSupportMaterial;
    def->tooltip = L("Create a brim per object instead of a brim for the plater."
        " Useful for complete_object or if you have your brim detaching before printing the object."
        "\nBe aware that the brim may be truncated if objects are too close together.");
    def->mode = comAdvancedE | comSuSi;
    def->set_default_value(new ConfigOptionBool(false));

    def = definition.add("brim_width", coFloat, ptFFF);
    def->label = L("Brim width");
    def->category = OptionCategory::skirtBrim;
    def->invalidates_step = posSlice;
    def->tooltip = L("Horizontal width of the brim that will be printed around each object on the first layer."
        "\nWhen raft is used, no brim is generated (use raft_first_layer_expansion).");
    def->sidetext = L("mm");
    def->min = 0;
    def->mode = comSimpleAE | comPrusa;
    def->set_default_value(new ConfigOptionFloat(0));

    def = definition.add("brim_width_interior", coFloat, ptFFF);
    def->label = L("Interior Brim width");
    def->category = OptionCategory::skirtBrim;
    def->invalidates_step = posSlice;
    def->tooltip = L("Horizontal width of the brim that will be printed inside each object on the first layer.");
    def->sidetext = L("mm");
    def->min = 0;
    def->mode = comAdvancedE | comSuSi;
    def->set_default_value(new ConfigOptionFloat(0));

    def = definition.add("brim_acceleration", coFloatOrPercent, ptFFF);
    def->label = L("Brim & Skirt");
    def->full_label = L("Brim & Skirt acceleration");
    def->category = OptionCategory::speed;
    def->invalidates_step = psGCodeExport;
    def->tooltip = L("This is the acceleration your printer will use for brim and skirt. "
        "\nCan be a % of the support acceleration"
        "\nSet zero to use support acceleration.");
    def->sidetext = L("mm/s² or %");
    def->ratio_over = "support_material_acceleration";
    def->min = 0;
    def->max_literal = { -200, false };
    def->mode = comAdvancedE | comSuSi;
    def->set_default_value(new ConfigOptionFloatOrPercent(0, false));

    def = definition.add("brim_separation", coFloat, ptFFF);
    def->label = L("Brim separation gap");
    def->category = OptionCategory::skirtBrim;
    def->invalidates_step = posSupportMaterial;
    def->tooltip = L("Offset of brim from the printed object. Should be kept at 0 unless you encounter great difficulties to separate them."
        "\nIt's subtracted to brim_width and brim_width_interior, so it has to be lower than them. The offset is applied after the first layer XY compensation (elephant foot).");
    def->sidetext = L("mm");
    def->mode = comExpert | comPrusa;
    def->set_default_value(new ConfigOptionFloat(0));
    def->aliases = { "brim_offset" }; // from superslicer 2.3

    def = definition.add("brim_speed", coFloatOrPercent, ptFFF);
    def->label = L("Brim & Skirt");
    def->full_label = L("Brim & Skirt speed");
    def->category = OptionCategory::speed;
    def->invalidates_step = psGCodeExport;
    def->tooltip = L("This separate setting will affect the speed of brim and skirt. "
        "\nIf expressed as percentage (for example: 80%) it will be calculated over the Support speed setting."
        "\nSet zero to use autospeed for this feature.");
    def->sidetext = L("mm/s or %");
    def->ratio_over = "support_material_speed";
    def->min = 0;
    def->mode = comExpert | comSuSi;
    def->set_default_value(new ConfigOptionFloatOrPercent(50, true));

#if 0
    def = definition.add("brim_type", coEnum, ptFFF);
    def->label = L("Brim type");
    def->category = L("Skirt and brim");
    def->invalidates_step = posSupportMaterial;
    def->tooltip = L("The places where the brim will be printed around each object on the first layer.");
    def->set_enum<BrimType>({
        { "no_brim",         L("No brim") },
        { "outer_only",      L("Outer brim only") },
        { "inner_only",      L("Inner brim only") },
        { "outer_and_inner", L("Outer and inner brim") } 
    });
    def->mode = comSimpleAE | comPrusa;
    def->set_default_value(new ConfigOptionEnum<BrimType>(btOuterOnly));
#endif

    def = definition.add("chamber_temperature", coInts, ptFFF);
    def->label = L("Chamber");
    def->full_label = L("Chamber temperature");
    def->category = OptionCategory::cooling;
    def->invalidates_step = psGCodeExport;
    def->tooltip = L("Chamber temperature.");
    def->sidetext = L("°C");
    def->min = 0;
    def->max = 300;
    def->mode = comExpert | comSuSi;
    def->is_vector_extruder = true;
    def->set_default_value(new ConfigOptionInts{ 0 });

    def = definition.add("colorprint_heights", coFloats, ptFFF);
    def->label = L("Colorprint height");
    def->category = OptionCategory::slicing;
    def->invalidates_step = psGCodeExport;
    def->mode = comExpert | comPrusa; // note: hidden setting
    def->tooltip = L("Heights at which a filament change is to occur. ");
    def->set_default_value(new ConfigOptionFloats { });

    def = definition.add("compatible_printers", coStrings, ptFFF);
    def->label = L("Compatible printers");
    def->mode = comAdvancedE | comPrusa;
    def->set_default_value(new ConfigOptionStrings());
    def->cli = ConfigOptionDef::nocli;

    def = definition.add("compatible_printers_condition", coString, ptFFF);
    def->label = L("Compatible printers condition");
    def->tooltip = L("A boolean expression using the configuration values of an active printer profile. "
                   "If this expression evaluates to true, this profile is considered compatible "
                   "with the active printer profile.");
    def->mode = comExpert | comPrusa;
    def->set_default_value(new ConfigOptionString());
    def->cli = ConfigOptionDef::nocli;

    def = definition.add("compatible_prints", coStrings, ptFFF);
    def->label = L("Compatible print profiles");
    def->mode = comAdvancedE | comPrusa;
    def->set_default_value(new ConfigOptionStrings());
    def->cli = ConfigOptionDef::nocli;

    def = definition.add("compatible_prints_condition", coString, ptFFF);
    def->label = L("Compatible print profiles condition");
    def->tooltip = L("A boolean expression using the configuration values of an active print profile. "
                   "If this expression evaluates to true, this profile is considered compatible "
                   "with the active print profile.");
    def->mode = comExpert | comPrusa;
    def->set_default_value(new ConfigOptionString());
    def->cli = ConfigOptionDef::nocli;

    // The following value is to be stored into the project file (AMF, 3MF, Config ...)
    // and it contains a sum of "compatible_printers_condition" values over the print and filament profiles.
    def = definition.add("compatible_printers_condition_cummulative", coStrings, ptFFF);
    def->set_default_value(new ConfigOptionStrings());
    def->cli = ConfigOptionDef::nocli;
    def->mode = comNone | comPrusa; // note: hidden setting
    def = definition.add("compatible_prints_condition_cummulative", coStrings, ptFFF);
    def->set_default_value(new ConfigOptionStrings());
    def->cli = ConfigOptionDef::nocli;
    def->mode = comNone | comPrusa; // note: hidden setting

    def = definition.add("complete_objects", coBool, ptFFF);
    def->label = L("Complete individual objects");
    def->category = OptionCategory::output;
    def->invalidates_step = psSkirtBrim;
    def->tooltip = L("When printing multiple objects or copies, this feature will complete "
        "each object before moving onto next one (and starting it from its bottom layer). "
        "This feature is useful to avoid the risk of ruined prints. "
        "Slic3r should warn and prevent you from extruder collisions, but beware.");
    def->mode = comSimpleAE | comPrusa;
    def->set_default_value(new ConfigOptionBool(false));

    def = definition.add("complete_objects_one_skirt", coBool, ptFFF);
    def->label = L("Allow only one skirt loop");
    def->category = OptionCategory::output;
    def->invalidates_step = psSkirtBrim;
    def->tooltip = L("When using 'Complete individual objects', the default behavior is to draw the skirt around each object."
        " if you prefer to have only one skirt for the whole platter, use this option.");
    def->mode = comAdvancedE | comSuSi;
    def->set_default_value(new ConfigOptionBool(false));

    def = definition.add("complete_objects_sort", coEnum, ptFFF);
    def->label = L("Object sort");
    def->category = OptionCategory::output;
    def->invalidates_step = psGCodeExport;
    def->tooltip = L("When printing multiple objects or copies on after another, this will help you to choose how it's ordered."
        "\nObject will sort them by the order of the right panel."
        "\nLowest Y will sort them by their lowest Y point. Useful for printers with a X-bar."
        "\nLowest Z will sort them by their height, useful for delta printers."
        "\nNearest will try to jump to the nearest.");
    def->mode = comAdvancedE | comSuSi;
    def->set_enum<CompleteObjectSort>({
        { "object", L("Right panel") },
        { "lowy", L("lowest Y") },
        { "lowz", L("lowest Z") },
        { "nearest", L("Nearest") },
    });
    def->set_default_value(new ConfigOptionEnum<CompleteObjectSort>(cosObject));

#if 0
    //not used anymore, to remove !! @DEPRECATED
    def = definition.add("cooling", coBools, ptFFF);
    def->label = L("Enable auto cooling");
    def->category = OptionCategory::cooling;
    def->tooltip = L("This flag enables the automatic cooling logic that adjusts print speed "
                   "and fan speed according to layer printing time.");
    def->mode = comAdvancedE | comPrusa;
    def->is_vector_extruder = true;
    def->set_default_value(new ConfigOptionBools { true });
#endif

    def = definition.add("cooling_tube_retraction", coFloat, ptFFF);
    def->label = L("Cooling tube position");
    def->category = OptionCategory::mmsetup;
    def->invalidates_step = psSkirtBrim;
    def->tooltip = L("Distance of the center-point of the cooling tube from the extruder tip.");
    def->sidetext = L("mm");
    def->min = 0;
    def->mode = comAdvancedE | comPrusa;
    def->set_default_value(new ConfigOptionFloat(91.5));

    def = definition.add("cooling_tube_length", coFloat, ptFFF);
    def->label = L("Cooling tube length");
    def->category = OptionCategory::mmsetup;
    def->invalidates_step = psSkirtBrim;
    def->tooltip = L("Length of the cooling tube to limit space for cooling moves inside it.");
    def->sidetext = L("mm");
    def->min = 0;
    def->mode = comAdvancedE | comPrusa;
    def->set_default_value(new ConfigOptionFloat(5.));

    def = definition.add("curve_smoothing_angle_convex", coFloat, ptFFF);
    def->label = L("Min convex angle");
    def->full_label = L("Curve smoothing minimum angle (convex)");
    def->category = OptionCategory::slicing;
    def->invalidates_step = posSlice;
    def->tooltip = L("Minimum (convex) angle at a vertex to enable smoothing"
        " (trying to create a curve around the vertex). "
        "180 : nothing will be smooth, 0 : all angles will be smoothened.");
    def->sidetext = L("°");
    def->aliases = { "curve_smoothing_angle" };
    def->cli = "curve-smoothing-angle-convex=f";
    def->min = 0;
    def->max = 180;
    def->mode = comAdvancedE | comSuSi;
    def->set_default_value(new ConfigOptionFloat(0));

    def = definition.add("curve_smoothing_angle_concave", coFloat, ptFFF);
    def->label = L("Min concave angle");
    def->full_label = L("Curve smoothing minimum angle (concave)");
    def->category = OptionCategory::slicing;
    def->invalidates_step = posSlice;
    def->tooltip = L("Minimum (concave) angle at a vertex to enable smoothing"
        " (trying to create a curve around the vertex). "
        "180 : nothing will be smooth, 0 : all angles will be smoothened.");
    def->sidetext = L("°");
    def->cli = "curve-smoothing-angle-concave=f";
    def->min = 0;
    def->max = 180;
    def->mode = comAdvancedE | comSuSi;
    def->set_default_value(new ConfigOptionFloat(0));

    def = definition.add("curve_smoothing_precision", coFloat, ptFFF);
    def->label = L("Precision");
    def->full_label = L("Curve smoothing precision");
    def->category = OptionCategory::slicing;
    def->invalidates_step = posSlice;
    def->tooltip = L("These parameters allow the slicer to smooth the angles in each layer. "
        "The precision will be at least the new precision of the curve. Set to 0 to deactivate."
        "\nNote: as it uses the polygon's edges and only works in the 2D planes, "
        "you must have a very clean or hand-made 3D model."
        "\nIt's really only useful to smoothen functional models or very wide angles.");
    def->sidetext = L("mm");
    def->min = 0;
    def->precision = 8;
    def->cli = "curve-smoothing-precision=f";
    def->mode = comAdvancedE | comSuSi;
    def->set_default_value(new ConfigOptionFloat(0));

    def = definition.add("curve_smoothing_cutoff_dist", coFloat, ptFFF);
    def->label = L("cutoff");
    def->full_label = L("Curve smoothing cutoff dist");
    def->category = OptionCategory::slicing;
    def->invalidates_step = posSlice;
    def->tooltip = L("Maximum distance between two points to allow adding new ones. Allow to avoid distorting long straight areas.\nSet zero to disable.");
    def->sidetext = L("mm");
    def->min = 0;
    def->cli = "curve-smoothing-cutoff-dist=f";
    def->mode = comAdvancedE | comSuSi;
    def->set_default_value(new ConfigOptionFloat(2));

    def = definition.add("default_acceleration", coFloatOrPercent, ptFFF);
    def->label = L("Default");
    def->category = OptionCategory::speed;
    def->invalidates_step = psGCodeExport;
    def->full_label = L("Default acceleration");
    def->tooltip = L("This is the acceleration your printer will be reset to after "
        "the role-specific acceleration values are used (perimeter/infill). "
        "\nAccelerations from the left column can also be expressed as a percentage of this value."
        "\nThis can be expressed as a percentage (for example: 80%) over the machine Max Acceleration for X axis."
        "\nSet zero to prevent resetting acceleration at all.");
    def->sidetext = L("mm/s² or %");
    def->ratio_over = "machine_max_acceleration_x";
    def->min = 0;
    def->max_literal = { -200, false };
    def->mode = comAdvancedE | comPrusa;
    def->set_default_value(new ConfigOptionFloatOrPercent(0, false));

    def = definition.add("default_filament_profile", coStrings, ptFFF);
    def->label = L("Default filament profile");
    def->tooltip = L("Default filament profile associated with the current printer profile. "
                   "On selection of the current printer profile, this filament profile will be activated.");
    def->mode = comNone | comPrusa; // note: hidden setting
    def->set_default_value(new ConfigOptionStrings());
    def->cli = ConfigOptionDef::nocli;

    def           = definition.add("default_fan_speed", coInts, ptFFF);
    def->label    = L("Default fan speed");
    def->category = OptionCategory::cooling;
    def->invalidates_step = psGCodeExport;
    def->tooltip  = L(
        "Default speed for the fan, to set the speed for features where there is no fan control. Useful for PLA and other low-temp filament."
        "\nSet 0 to disable the fan by default. Useful for ABS and other high-temp filaments."
        "\nIf disabled, no fan speed command will be emitted when possible (if a feature set a speed, it won't be reverted).");
    def->mode               = comSimpleAE | comSuSi;
    def->min                = 0;
    def->max                = 100;
    def->is_vector_extruder = true;
    def->can_be_disabled = true;
    def->set_default_value(disable_default_option(new ConfigOptionInts({ 100 })));
    def->aliases = { "min_fan_speed" }; // only if "fan_always_on"

    def = definition.add("default_print_profile", coString, ptFFF);
    def->label = L("Default print profile");
    def->tooltip = L("Default print profile associated with the current printer profile. "
                   "On selection of the current printer profile, this print profile will be activated.");
    def->mode = comNone | comPrusa; // note: hidden setting
    def->set_default_value(new ConfigOptionString());
    def->cli = ConfigOptionDef::nocli;

    def = definition.add("default_speed", coFloatOrPercent, ptFFF);
    def->label = L("Default");
    def->category = OptionCategory::speed;
    def->invalidates_step = psGCodeExport;
    def->full_label = L("Default speed");
    def->tooltip = L("This is the reference speed that other 'main' speed can reference to by a %."
        "\nThis setting doesn't do anything by itself, and so is deactivated unless a speed depends on it (a % from the left column)."
        "\nThis can be expressed as a percentage (for example: 80%) over the machine Max Feedrate for X axis."
        "\nSet zero to use autospeed for speed fields using a % of this setting.");
    def->sidetext = L("mm/s for %-based speed");
    def->sidetext_width = 40;
    def->ratio_over = "machine_max_feedrate_x";
    def->min = 0;
    def->mode = comAdvancedE | comSuSi;
    def->set_default_value(new ConfigOptionFloatOrPercent(100, false));

    def = definition.add("disable_fan_first_layers", coInts, ptFFF);
    def->label = L("Disable fan for the first");
    def->category = OptionCategory::cooling;
    def->invalidates_step = psGCodeExport;
    def->tooltip = L("You can set this to a positive value to disable fan at all "
                   "during the first layers, so that it does not make adhesion worse.");
    def->sidetext = L("layers");
    def->min = 0;
    def->max = 1000;
    def->mode = comExpert | comPrusa;
    def->is_vector_extruder = true;
    def->set_default_value(new ConfigOptionInts { 1 });

    def = definition.add("draft_shield", coEnum, ptFFF);
    def->label = L("Draft shield");
    def->category = OptionCategory::skirtBrim;
    def->invalidates_step = psSkirtBrim;
    def->tooltip = L("With draft shield active, the skirt will be printed skirt_distance from the object, possibly intersecting brim.\n"
        "Enabled = skirt is as tall as the highest printed object.\n"
        "Limited = skirt is as tall as specified by skirt_height.\n"
        "This is useful to protect an ABS or ASA print from warping and detaching from print bed due to wind draft.");
    def->set_enum<DraftShield>({
        { "disabled", L("Disabled") },
        { "limited", L("Limited") },
        { "enabled", L("Enabled") },
    });
    def->mode = comAdvancedE | comPrusa;
    def->set_default_value(new ConfigOptionEnum<DraftShield>(dsDisabled));

    def = definition.add("duplicate_distance", coFloat, ptFFF);
    def->label = L("Default distance between objects");
    def->category = OptionCategory::output;
    def->invalidates_step = psGCodeExport;
    def->tooltip = L("Default distance used for the auto-arrange feature of the platter.\nSet to 0 to use the last value instead.");
    def->sidetext = L("mm");
    def->aliases = { "multiply_distance" };
    def->min = 0;
    def->mode = comExpert | comPrusa | comSuSi;
    def->set_default_value(new ConfigOptionFloat(6));

    def = definition.add("end_gcode", coString, ptFFF);
    def->label = L("End G-code");
    def->category = OptionCategory::customgcode;
    def->invalidates_step = psGCodeExport;
    def->tooltip = L("This end procedure is inserted at the end of the output file. "
                   "Note that you can use placeholder variables for all Slic3r settings.");
    def->multiline = true;
    def->full_width = true;
    def->height = 12;
    def->mode = comExpert | comPrusa;
    def->set_default_value(new ConfigOptionString("M104 S0 ; turn off temperature\nG28 X0  ; home X axis\nM84     ; disable motors\n"));

    def = definition.add("end_filament_gcode", coStrings, ptFFF);
    def->label = L("End G-code");
    def->full_label = L("Filament end G-code");
    def->category = OptionCategory::customgcode;
    def->invalidates_step = psGCodeExport;
    def->tooltip = L("This end procedure is inserted at the end of the output file, before the printer end gcode (and "
                   "before any toolchange from this filament in case of multimaterial printers). "
                   "Note that you can use placeholder variables for all Slic3r settings. "
                   "If you have multiple extruders, the gcode is processed in extruder order.");
    def->multiline = true;
    def->full_width = true;
    def->height = 120;
    def->mode = comExpert | comPrusa;
    def->is_vector_extruder = true;
    def->set_default_value(new ConfigOptionStrings { "; Filament-specific end gcode \n;END gcode for filament\n" });

    def = definition.add("top_fill_pattern", coEnum, ptFFF);
    def->label = L("Top");
    def->full_label = L("Top fill Pattern");
    def->category = OptionCategory::infill;
    def->invalidates_step = posInfill;
    def->tooltip = L("Fill pattern for top infill. This only affects the top visible layer, and not its adjacent solid shells."
        "\nIf you want an 'aligned' pattern, set 90° to the fill angle increment setting.");
    def->cli = "top-fill-pattern|external-fill-pattern=s";
    def->aliases = { "external_fill_pattern" };
    def->set_enum<InfillPattern>({
        { "rectilinear",        L("Rectilinear") },
        { "monotonic",          L("Monotonic") },
        { "monotonicgapfill",   L("Monotonic (filled)") },
        { "monotoniclines",     L("Monotonic Lines") },
        { "alignedrectilinear", L("Aligned Rectilinear") },
        { "concentric",         L("Concentric") },
        { "concentricgapfill",  L("Concentric (filled)") },
        { "hilbertcurve",       L("Hilbert Curve") },
        { "archimedeanchords",  L("Archimedean Chords") },
        { "octagramspiral",     L("Octagram Spiral") },
        { "sawtooth",     L("Sawtooth") },
        { "smooth",     L("Ironing") },
    });
    def->mode = comSimpleAE | comPrusa;
    def->set_default_value(new ConfigOptionEnum<InfillPattern>(ipMonotonic));

    def = definition.add("bottom_fill_pattern", coEnum, ptFFF);
    def->label = L("Bottom");
    def->full_label = L("Bottom fill pattern");
    def->category = OptionCategory::infill;
    def->invalidates_step = posInfill;
    def->tooltip = L("Fill pattern for bottom infill. This only affects the bottom visible layer, and not its adjacent solid shells."
        "\nIf you want an 'aligned' pattern, set 90° to the fill angle increment setting.");
    def->cli = "bottom-fill-pattern|external-fill-pattern=s";
    def->aliases = { "external_fill_pattern" };
    def->set_enum<InfillPattern>({
        { "rectilinear",        L("Rectilinear") },
        { "monotonic",          L("Monotonic") },
        { "monotonicgapfill",   L("Monotonic (filled)") },
        { "monotoniclines",     L("Monotonic Lines") },
        { "alignedrectilinear", L("Aligned Rectilinear") },
        { "concentric",         L("Concentric") },
        { "concentricgapfill",  L("Concentric (filled)") },
        { "hilbertcurve",       L("Hilbert Curve") },
        { "archimedeanchords",  L("Archimedean Chords") },
        { "octagramspiral",     L("Octagram Spiral") },
        { "smooth",     L("Ironing") },
    });

    def->mode = comAdvancedE | comPrusa;
    def->set_default_value(new ConfigOptionEnum<InfillPattern>(ipMonotonic));

    def = definition.add("solid_fill_pattern", coEnum, ptFFF);
    def->label = L("Solid fill pattern");
    def->category = OptionCategory::infill;
    def->invalidates_step = posInfill;
    def->tooltip = L("Fill pattern for solid (internal) infill. This only affects the solid not-visible layers. You should use rectilinear in most cases. You can try ironing for translucent material."
        " Rectilinear (filled) replaces zig-zag patterns by a single big line & is more efficient for filling little spaces."
        "\nIf you want an 'aligned' pattern, set 90° to the fill angle increment setting.");
    def->set_enum<InfillPattern>({
        { "ensuring",           L("Ensuring") },
        { "rectilinear",        L("Rectilinear") },
        { "rectilineargapfill", L("Rectilinear (filled)") },
        { "monotonic",          L("Monotonic") },
        { "monotonicgapfill",   L("Monotonic (filled)") },
        { "monotoniclines",     L("Monotonic Lines") },
        { "alignedrectilinear", L("Aligned Rectilinear") },
        { "concentric",         L("Concentric") },
        { "concentricgapfill",  L("Concentric (filled)") },
        { "hilbertcurve",       L("Hilbert Curve") },
        { "archimedeanchords",  L("Archimedean Chords") },
        { "octagramspiral",     L("Octagram Spiral") },
        { "smooth",             L("Ironing") },
    });

    def->mode = comExpert | comSuSi;
    def->set_default_value(new ConfigOptionEnum<InfillPattern>(ipEnsuring));

    def = definition.add("enforce_full_fill_volume", coBool, ptFFF);
    def->label = L("Enforce 100% fill volume");
    def->category = OptionCategory::infill;
    def->invalidates_step = posInfill;
    def->tooltip = L("Experimental option which modifies (in solid infill) fill flow to have the exact amount of plastic inside the volume to fill "
        "(it generally changes the flow from -7% to +4%, depending on the size of the surface to fill and the overlap parameters, "
        "but it can go as high as +50% for infill in very small areas where rectilinear doesn't have good coverage). It has the advantage "
        "to remove the over-extrusion seen in thin infill areas, from the overlap ratio");
    def->mode = comExpert | comSuSi;
    def->set_default_value(new ConfigOptionBool(true));

    def = definition.add("enforce_retract_first_layer", coBool, ptFFF);
    def->label = L("But on first layer");
    def->full_label = L("Don't check crossings for retraction on first layer");
    def->category = OptionCategory::extruders;
    def->invalidates_step = psGCodeExport;
    def->tooltip = L("let the retraction happens on the first layer even if the travel path does not exceed the upper layer's perimeters.");
    def->mode = comExpert | comPrusa;
    def->set_default_value(new ConfigOptionBool(true));

    def = definition.add("ensure_vertical_shell_thickness", coEnum, ptFFF);
    def->label = L("Ensure vertical shell thickness");
    def->category = OptionCategory::perimeter;
    def->invalidates_step = posPrepareInfill;
    def->tooltip = L("Add solid infill near sloping surfaces to guarantee the vertical shell thickness "
                   "(top+bottom solid layers).");
    def->set_enum<EnsureVerticalShellThickness>({
        { "disabled", L("Disabled (2.5)") },
        { "partial",  L("partial (2.9 experimental)")  },
        { "enabled",  L("Enabled (2.7 experimental)")  },
        { "enabled_old",  L("Enabled (2.5)")  },
    });
    def->mode = comAdvancedE | comPrusa;
    def->set_default_value(new ConfigOptionEnum<EnsureVerticalShellThickness>(EnsureVerticalShellThickness::Enabled_old));

    def = definition.add("external_infill_margin", coFloatOrPercent, ptFFF);
    def->label = L("Default");
    def->full_label = L("Default infill margin");
    def->category = OptionCategory::infill;
    def->invalidates_step = posPerimeters;
    def->tooltip = L("This parameter grows the top/bottom/solid layers by the specified mm to anchor them into the sparse infill and support the perimeters above."
        " Put 0 to deactivate it. Can be a % of the width of the perimeters.");
    def->sidetext = L("mm or %");
    def->ratio_over = "perimeter_extrusion_width";
    def->min = 0;
    def->max_literal = { 50, true };
    def->mode = comExpert | comSuSi;
    def->set_default_value(new ConfigOptionFloatOrPercent(150, true));

    def = definition.add("external_perimeter_extrusion_width", coFloatOrPercent, ptFFF);
    def->label = L("External perimeters");
    def->full_label = L("External perimeters width");
    def->category = OptionCategory::width;
    def->invalidates_step = posSlice;
    def->tooltip = L("Set this to a non-zero value to set a manual extrusion width for external perimeters. "
        "If left zero, default extrusion width will be used if set, otherwise 1.05 x nozzle diameter will be used. "
        "If expressed as percentage (for example 112.5%), it will be computed over nozzle diameter."
        "\nYou can set either 'Spacing', or 'Width'; the other will be calculated, using the perimeter 'Overlap' percentages and default layer height.");
    def->sidetext = L("mm or %");
    def->ratio_over = "nozzle_diameter";
    def->min = 0;
    def->max = 1000;
    def->max_literal = { 10, true };
    def->precision = 6;
    def->can_phony = true;
    def->mode = comAdvancedE | comPrusa;
    def->set_default_value(new ConfigOptionFloatOrPercent(105, true));

    def = definition.add("external_perimeter_extrusion_spacing", coFloatOrPercent, ptFFF);
    def->label = L("External perimeters");
    def->full_label = L("External perimeters spacing");
    def->category = OptionCategory::width;
    def->invalidates_step = posSlice;
    def->tooltip = L("Like the External perimeters width, but this value is the distance between the edge and the 'frontier' to the next perimeter."
                "\nSetting the spacing will deactivate the width setting, and vice versa."
                "\nYou can set either 'Spacing', or 'Width'; the other will be calculated, using the perimeter 'Overlap' percentages and default layer height.");
    def->sidetext = L("mm or %");
    def->ratio_over = "nozzle_diameter";
    def->min = 0;
    def->max = 1000;
    def->max_literal = { 10, true };
    def->precision = 6;
    def->can_phony = true;
    def->mode = comAdvancedE | comSuSi;
    def->set_default_value((new ConfigOptionFloatOrPercent(0, false))->set_phony(true));

    def = definition.add("external_perimeter_extrusion_change_odd_layers", coFloatOrPercent, ptFFF);
    def->label = L("External perimeters");
    def->full_label = L("External perimeters spacing change on even layers");
    def->category = OptionCategory::width;
    def->invalidates_step = posPerimeters;
    def->tooltip = L("Change width on every even layer (and not on odd layers like the first one) for better overlap with adjacent layers and getting stringer shells. "
                     "Try values about +/- 0.1 with different sign for external and internal perimeters."
                     "\nThis could be combined with extra permeters on even layers."
                     "\nWorks as absolute spacing or a % of the spacing."
                     "\nset 0 to disable");
    def->sidetext = L("mm or %");
    def->mode = comExpert | comSuSi;
    def->set_default_value(new ConfigOptionFloatOrPercent(false, 0));

    def = definition.add("external_perimeter_cut_corners", coPercent, ptFFF);
    def->label = L("Cutting corners");
    def->full_label = L("Ext. peri. cut corners");
    def->category = OptionCategory::width;
    def->invalidates_step = psGCodeExport;
    def->tooltip = L("Activate this option to modify the flow to acknowledge that the nozzle is round and the corners will have a round shape, and so change the flow to realize that and avoid over-extrusion."
        " 100% is activated, 0% is deactivated and 50% is half-activated."
        "\nNote: At 100% this changes the flow by ~5% over a very small distance (~nozzle diameter), so it shouldn't be noticeable unless you have a very big nozzle and a very precise printer.");
    def->sidetext = L("%");
    def->mode = comExpert | comSuSi;
    def->set_default_value(new ConfigOptionPercent(0));

    def = definition.add("external_perimeter_fan_speed", coInts, ptFFF);
    def->label = L("External perimeter fan speed");
    def->invalidates_step = psGCodeExport;
    def->tooltip = L("When set to a non-zero value this fan speed is used only for external perimeters (visible ones) and thin walls."
                    "\nSet to 0 to stop the fan."
                    "\nIf disabled, the default fan speed will be used."
                    "\nExternal perimeters can benefit from higher fan speed to improve surface finish, "
                    "while internal perimeters, infill, etc. benefit from lower fan speed to improve layer adhesion."
                    "\nCan be disabled by disable_fan_first_layers, slowed down by full_fan_speed_layer and increased by low layer time.");
    def->sidetext = L("%");
    def->min = 0;
    def->max = 100;
    def->mode = comAdvancedE | comSuSi;
    def->is_vector_extruder = true;
    def->can_be_disabled = true;
    def->set_default_value(disable_default_option(new ConfigOptionInts({ 100 })));

    def = definition.add("external_perimeter_overlap", coPercent, ptFFF);
    def->label = L("external perimeter overlap");
    def->full_label = L("Ext. peri. overlap");
    def->category = OptionCategory::width;
    def->invalidates_step = posPerimeters;
    def->tooltip = L("This setting allows you to reduce the overlap between the perimeters and the external one, to reduce the impact of the perimeters' artifacts."
        " 100% means that no gap is left, and 0% means that the external perimeter isn't contributing to the overlap with the 'inner' one.");
    def->sidetext = L("%");
    def->min = 0;
    def->max = 100;
    def->mode = comExpert | comSuSi;
    def->set_default_value(new ConfigOptionPercent(80));

    def = definition.add("external_perimeter_acceleration", coFloatOrPercent, ptFFF);
    def->label = L("External");
    def->full_label = L("External Perimeter acceleration");
    def->category = OptionCategory::speed;
    def->invalidates_step = psGCodeExport;
    def->tooltip = L("This is the acceleration your printer will use for external perimeters. "
                "\nCan be a % of the internal perimeter acceleration"
                "\nSet zero to use internal perimeter acceleration for external perimeters.");
    def->sidetext = L("mm/s² or %");
    def->ratio_over = "perimeter_acceleration";
    def->min = 0;
    def->max_literal = { -200, false };
    def->mode = comExpert | comPrusa;
    def->set_default_value(new ConfigOptionFloatOrPercent(0,false));

    def = definition.add("external_perimeter_speed", coFloatOrPercent, ptFFF);
    def->label = L("External");
    def->full_label = L("External perimeters speed");
    def->category = OptionCategory::speed;
    def->invalidates_step = psGCodeExport;
    def->tooltip = L("This separate setting will affect the speed of external perimeters (the visible ones). "
                   "\nIf expressed as percentage (for example: 80%) it will be calculated over the Internal Perimeters speed setting."
                   "\nSet zero to use autospeed for this feature.");
    def->sidetext = L("mm/s or %");
    def->ratio_over = "perimeter_speed";
    def->min = 0;
    def->mode = comExpert | comPrusa;
    def->set_default_value(new ConfigOptionFloatOrPercent(50, true));

    def = definition.add("external_perimeters_first", coBool, ptFFF);
    def->label = L("first");
    def->full_label = L("External perimeters first");
    def->category = OptionCategory::perimeter;
    def->invalidates_step = posPerimeters;
    def->tooltip = L("Print contour perimeters from the outermost one to the innermost one "
        "instead of the default inverse order.");
    def->mode = comExpert | comPrusa;
    def->set_default_value(new ConfigOptionBool(false));

    def = definition.add("external_perimeters_first_force", coBool, ptFFF);
    def->label = L("force for all");
    def->full_label = L("External perimeters first: force for all");
    def->category = OptionCategory::perimeter;
    def->invalidates_step = posPerimeters;
    def->tooltip = L("Print all external contours & perimeter first, then the internal ones.");
    def->mode = comExpert | comSuSi;
    def->set_default_value(new ConfigOptionBool(false));

    def = definition.add("seam_slope_type", coEnum, ptFFF);
    def->label = L("Scarf seam (In vase mode)");
    def->full_label = L("Scarf seam: activation");
    def->category = OptionCategory::perimeter;
    def->invalidates_step = psGCodeExport;
    def->tooltip = L("Print contour perimeters in two circles, in a continuous way, like for a vase mode. It needs the external_perimeters_first parameter to work."
        "\nDoesn't work for the first layer, as it may damage the bed overwise."
        "\nIt does two loop instead of one, the first one growing and the second one shrinking the height.");
    def->set_enum<SeamScarfType>({
        { "none", L("None") },
        { "external",  L("Contours")  },
        { "all",  L("Contours and holes")  },
    });
    def->mode = comAdvancedE;
    def->set_default_value(new ConfigOptionEnum<SeamScarfType>(SeamScarfType::None));

    def = definition.add("seam_slope_min_height", coFloatOrPercent, ptFFF);
    def->label = L("Minimum extrusion height");
    def->full_label = L("Scarf seam: Minimum extrusion height");
    def->category = OptionCategory::perimeter;
    def->invalidates_step = psGCodeExport;
    def->tooltip = L("When using the scarf seam ('seam_slope_type'), it will use this setting to compute the base height (it doesn't start at 0)"
        ", so be sure to put here the lowest value your extruder can handle without clogging (or 0 if it works)."
        "\n The height is clamped to a third of the current layer height, you can't go higher or you won't be able to do a scarf at all."
        "\nCan be a percentage of the current nozzle diameter.");
    def->mode = comExpert;
    def->aliases = {"external_perimeters_vase_min_height"};
    def->set_default_value(new ConfigOptionFloatOrPercent(5, true));

    def = definition.add("seam_slope_max_length", coFloatOrPercent, ptFFF);
    def->label = L("Maximum length");
    def->full_label = L("Scarf seam: Maximum length");
    def->invalidates_step = psGCodeExport;
    def->tooltip = L("Length of the scarf seam. Disable to make the scarf the entire loop."
        "\nCan be a percentage of the current nozzle diameter.");
    def->sidetext = L("mm");
    def->min = 0;
    def->can_be_disabled = true;
    def->mode = comAdvancedE;
    def->set_default_value(enable_default_option(new ConfigOptionFloatOrPercent(20, false)));

    def = definition.add("external_perimeters_nothole", coBool, ptFFF);
    def->label = L("Only for contours");
    def->full_label = L("Ext peri first for outer side");
    def->category = OptionCategory::perimeter;
    def->invalidates_step = posPerimeters;
    def->tooltip = L("Only do the vase trick on the external side. Useful when the thickness is too low.");
    def->mode = comExpert | comSuSi;
    def->set_default_value(new ConfigOptionBool(true));

    def = definition.add("external_perimeters_hole", coBool, ptFFF);
    def->label = L("Only for holes");
    def->full_label = L("ext peri first for inner side");
    def->category = OptionCategory::perimeter;
    def->invalidates_step = posPerimeters;
    def->tooltip = L("Only do the vase trick on the external side. Useful when you only want to remove seam from screw hole.");
    def->mode = comExpert | comSuSi;
    def->set_default_value(new ConfigOptionBool(true));

    def = definition.add("extra_perimeters", coBool, ptFFF);
    def->label = L("filling horizontal gaps on slopes");
    def->full_label = L("Add perimeters on slope (do nothing)");
    def->category = OptionCategory::perimeter;
    def->invalidates_step = posPerimeters;
    def->tooltip = L("Add more perimeters when needed for avoiding gaps in sloping walls. "
        "Slic3r keeps adding perimeters, until more than 70% of the loop immediately above "
        "is supported."
        "\nIf you succeed in triggering the algorithm behind this setting, please send me a message."
        " Personally, I think it's useless.");
    def->mode = comExpert | comPrusa;
    def->set_default_value(new ConfigOptionBool(false));

    def = definition.add("extra_perimeters_below_area", coFloatOrPercent, ptFFF);
    def->label = L("Extra perimeters on small areas");
    def->category = OptionCategory::perimeter;
    def->invalidates_step = posPerimeters;
    def->tooltip = L("After laying all the perimeters, if there is an area smaller (in mm²) than this value, then fill it with more perimeters."
                    "\nUseful if you want to fortify a small cylinder while not messing with the larger main object."
                    "\nCan be a percentage of the perimeter width (squared)."
                    "\nSet zero to disable.");
    def->mode = comAdvancedE | comSuSi;
    def->set_default_value(new ConfigOptionFloatOrPercent(0,false));

    def = definition.add("extra_perimeters_count", coInt, ptFFF);
    def->label = L("Extra perimeters");
    def->category = OptionCategory::perimeter;
    def->invalidates_step = posPerimeters;
    def->tooltip = L("To be used in modifiers, as long as perimeter contour & hole count split the perimeters.");
    def->mode = comAdvancedE | comSuSi;
    def->set_default_value(new ConfigOptionInt(0));

    def = definition.add("extra_perimeters_on_overhangs", coBool, ptFFF);
    def->label = L("Extra perimeters on overhangs (Experimental)");
    def->category = OptionCategory::perimeter;
    def->invalidates_step = posPerimeters;
    def->tooltip = L("Detect overhang areas where bridges cannot be anchored, and fill them with "
                    "extra perimeter paths. These paths are anchored to the nearby non-overhang area when possible."
                    "\nIf you use this setting, strongly consider also using overhangs_reverse.");
    def->aliases = {"extra_perimeters_overhangs"};
    def->mode = comAdvancedE | comPrusa;
    def->set_default_value(new ConfigOptionBool(false));

    def = definition.add("extra_perimeters_odd_layers", coBool, ptFFF);
    def->label = L("On even layers");
    def->full_label = L("Extra perimeter on even layers");
    def->category = OptionCategory::perimeter;
    def->invalidates_step = posPerimeters;
    def->tooltip = L("Adds one extra perimeter on alternating layers, allowing infill to be captured between "
                     "perimeter shells. This can significantly reduce how much infill needs to encroach into perimeters.");
    def->mode = comAdvancedE | comSuSi;
    def->set_default_value(new ConfigOptionBool(false));

    def = definition.add("extruder", coInt, ptFFF);
    def->label = L("Extruder");
    def->category = OptionCategory::extruders;
    def->tooltip = L("The extruder to use (unless more specific extruder settings are specified). "
        "This value overrides perimeter and infill extruders, but not the support extruders.");
    def->mode = comAdvancedE | comPrusa; // note: hidden setting
    def->min = 0;  // 0 = inherit defaults
    def->set_enum_labels(ConfigOptionDef::GUIType::i_enum_open, 
        { L("default"), "1", "2", "3", "4", "5", "6", "7", "8", "9" }); // override label for item 0


    def = definition.add("first_layer_extruder", coInt, ptFFF);
    def->gui_type = ConfigOptionDef::GUIType::i_enum_open;
    def->label = L("First layer extruder");
    def->category = OptionCategory::extruders;
    def->tooltip = L("The extruder to use (unless more specific extruder settings are specified) for the first layer.");
    def->min = 0;  // 0 = inherit defaults
    def->mode = comExpert | comSuSi;
    def->set_enum_labels(ConfigOptionDef::GUIType::i_enum_open, 
        { L("default"), "1", "2", "3", "4", "5", "6", "7", "8", "9" }); // override label for item 0

    def = definition.add("extruder_clearance_height", coFloat, ptFFF);
    def->label = L("Height");
    def->full_label = L("Extruder clearance height");
    def->category = OptionCategory::output;
    def->invalidates_step = psGCodeExport;
    def->tooltip = L("Set this to the vertical distance between your nozzle tip and (usually) the X carriage rods. "
                   "In other words, this is the height of the clearance cylinder around your extruder, "
                   "and it represents the maximum depth the extruder can peek before colliding with "
                   "other printed objects."); // TODO: "peek?" is this the correct word?
    def->sidetext = L("mm");
    def->min = 0;
    def->mode = comExpert | comPrusa;
    def->set_default_value(new ConfigOptionFloat(20));

    def             = definition.add("extruder_clearance", coGraphs, ptFFF);
    def->label      = L("Extruder clearance");
    def->category   = OptionCategory::speed;
    def->invalidates_step = psGCodeExport;
    def->tooltip    = L("height of the extruder in function of the clearance radius (all in mm).");
    def->is_vector_extruder = true;
    def->mode       = comExpert | comSuSi;
    def->set_default_value(new ConfigOptionGraphs({GraphData(0,3, GraphData::GraphType::LINEAR,
        {{0, 0},{0.55, 0},{2,1.9}}
    )}));
    def->graph_settings = std::make_shared<GraphSettings>();
    def->graph_settings->title       = L("Overhangs fan speed by % of overlap");
    def->graph_settings->description = L("Choose the Overhangs maximum fan speed for each percentage of overlap with the layer below."
        "If the current fan speed (from perimeter, external, of default) is higher, then this setting won't slow the fan."
        "\n100% overlap is when the extrusion is fully on top of the previous layer's extrusion."
        "\n0% overlap is when the extrusion centerline is at a distance of 'overhangs threshold for speed'(overhangs_bridge_threshold)"
        "\nfrom the nearest extrusion of the previous layer.");
    def->graph_settings->x_label     = L("radius clearance");
    def->graph_settings->y_label     = L("Height from nozzle tip");
    def->graph_settings->label_min_x = L("");
    def->graph_settings->label_max_x = L("Highest available clearance");
    def->graph_settings->label_min_y = L("");
    def->graph_settings->label_max_y = L("Max height with clearance");
    def->graph_settings->min_x       = 0;
    def->graph_settings->max_x       = 10;
    def->graph_settings->step_x      = .1;
    def->graph_settings->min_y       = 0;
    def->graph_settings->max_y       = 10;
    def->graph_settings->step_y      = .1;
    def->graph_settings->enforced_values = {{0.,0.}};
    def->graph_settings->allowed_types = {GraphData::GraphType::LINEAR, GraphData::GraphType::SQUARE};

    def = definition.add("extruder_clearance_radius", coFloat, ptFFF);
    def->label = L("Radius");
    def->category = OptionCategory::output;
    def->invalidates_step = psGCodeExport;
    def->full_label = L("Extruder clearance radius");
    def->tooltip = L("Set this to the clearance radius around your extruder. "
                   "If the extruder is not centered, choose the largest value for safety. "
                   "This setting is used to check for collisions and to display the graphical preview "
                   "in the platter."
                   "\nSet zero to disable clearance checking.");
    def->sidetext = L("mm");
    def->min = 0;
    def->mode = comExpert | comPrusa;
    def->set_default_value(new ConfigOptionFloat(20));

    def = definition.add("extruder_colour", coStrings, ptFFF);
    def->label = L("Extruder Color");
    def->category = OptionCategory::extruders;
    def->invalidates_step = psGCodeExport;
    def->tooltip = L("This is only used in Slic3r interface as a visual help.");
    def->gui_type = ConfigOptionDef::GUIType::color;
    // Empty string means no color assigned yet.
    def->mode = comAdvancedE | comPrusa;
    def->is_vector_extruder = true;
    def->set_default_value(new ConfigOptionStrings{ "" });

    def = definition.add("extruder_extrusion_multiplier_speed", coGraphs, ptFFF);
    def->label = L("Extrusion multiplier");
    def->invalidates_step = psGCodeExport;
    def->tooltip = L("This string is edited by a Dialog and contains extrusion multiplier for different speeds.");
    def->mode = comExpert | comSuSi;
    def->is_vector_extruder = true;
    def->set_default_value(new ConfigOptionGraphs({ GraphData(0,10, GraphData::GraphType::LINEAR,
        {{10,1.},{20,1.},{30,1.},{40,1.},{60,1.},{80,1.},{120,1.},{160,1.},{240,1.},{320,1.},{480,1.},{640,1.},{960,1.},{1280,1.}}
    )}));
    def->graph_settings = std::make_shared<GraphSettings>();
    def->graph_settings->title       = L("Extrusion multiplier per extrusion speed");
    def->graph_settings->description = L("Choose the extrusion multiplier value for multiple speeds.\nYou can add/remove points with a right click.");
    def->graph_settings->x_label     = L("Print speed (mm/s)");
    def->graph_settings->y_label     = L("Extrusion multiplier");
    def->graph_settings->null_label  = L("No compensation");
    def->graph_settings->label_min_x = L("Graph min speed");
    def->graph_settings->label_max_x = L("Graph max speed");
    def->graph_settings->label_min_y = L("Minimum flow");
    def->graph_settings->label_max_y = L("Maximum flow");
    def->graph_settings->min_x       = 10;
    def->graph_settings->max_x       = 2000;
    def->graph_settings->step_x      = 1.;
    def->graph_settings->min_y       = 0.1;
    def->graph_settings->max_y       = 2;
    def->graph_settings->step_y      = 0.1;
    def->graph_settings->allowed_types = {GraphData::GraphType::LINEAR, GraphData::GraphType::SQUARE};

    def = definition.add("extruder_offset", coPoints, ptFFF);
    def->label = L("Extruder offset");
    def->category = OptionCategory::extruders;
    def->invalidates_step = psGCodeExport;
    def->tooltip = L("If your firmware doesn't handle the extruder displacement you need the G-code "
        "to take it into account. This option lets you specify the displacement of each extruder "
        "with respect to the first one. It expects positive coordinates (they will be subtracted "
        "from the XY coordinate).");
    def->sidetext = L("mm");
    def->mode = comAdvancedE | comPrusa;
    def->is_vector_extruder = true;
    def->set_default_value(new ConfigOptionPoints{ Vec2d(0,0) });

    def = definition.add("extruder_temperature_offset", coFloats, ptFFF);
    def->label = L("Extruder temp offset");
    def->category = OptionCategory::extruders;
    def->invalidates_step = psGCodeExport;
    def->tooltip = L("This offset will be added to all extruder temperatures set in the filament settings."
        "\nNote that you should set 'M104 S{first_layer_temperature{initial_extruder} + extruder_temperature_offset{initial_extruder}}'"
        "\ninstead of 'M104 S{first_layer_temperature}' in the start_gcode");
    def->sidetext = L("°C");
    def->mode = comExpert | comSuSi;
    def->is_vector_extruder = true;
    def->set_default_value(new ConfigOptionFloats{ 0 });

    def = definition.add("extruder_fan_offset", coPercents, ptFFF);
    def->label = L("Extruder fan offset");
    def->category = OptionCategory::extruders;
    def->invalidates_step = psGCodeExport;
    def->tooltip = L("This offset wil be added to all fan values set in the filament properties. It won't make them go higher than 100% nor lower than 0%.");
    def->sidetext = L("%");
    def->mode = comExpert | comSuSi;
    def->is_vector_extruder = true;
    def->set_default_value(new ConfigOptionPercents{ 0 });


    def = definition.add("extrusion_axis", coString, ptFFF);
    def->label = L("Extrusion axis");
    def->category = OptionCategory::extruders;
    def->invalidates_step = psGCodeExport;
    def->tooltip = L("Use this option to set the axis letter associated with your printer's extruder "
                   "(usually E but some printers use A).");
    def->mode = comNone | comPrusa; // note: hidden setting
    def->set_default_value(new ConfigOptionString("E"));

    def = definition.add("extrusion_multiplier", coFloats, ptFFF);
    def->label = L("Extrusion multiplier");
    def->category = OptionCategory::filament;
    def->invalidates_step = psGCodeExport;
    def->tooltip = L("This factor changes the amount of flow proportionally. You may need to tweak "
        "this setting to get nice surface finish and correct single wall widths. "
        "Usual values are between 0.9 and 1.1. If you think you need to change this more, "
        "check filament diameter and your firmware E steps.");
    def->mode = comSimpleAE | comPrusa;
    def->min = 0;
    def->max = 2;
    def->is_vector_extruder = true;
    def->set_default_value(new ConfigOptionFloats { 1. });

    def = definition.add("print_extrusion_multiplier", coPercent, ptFFF);
    def->label = L("Extrusion multiplier");
    def->category = OptionCategory::filament;
    def->invalidates_step = psGCodeExport;
    def->tooltip = L("This factor changes the amount of flow proportionally. You may need to tweak "
        "this setting to get nice surface finish and correct single wall widths. "
        "Usual values are between 90% and 110%. If you think you need to change this more, "
        "check filament diameter and your firmware E steps."
        " This print setting is multiplied against the extrusion_multiplier from the filament tab."
        " Its only purpose is to offer the same functionality but on a per-object basis."); // TODO: replace "against" with "with"?
    def->sidetext = L("%");
    def->mode = comSimpleAE | comSuSi;
    def->min = 0;
    def->max = 200;
    def->set_default_value(new ConfigOptionPercent(100));

    def = definition.add("extrusion_width", coFloatOrPercent, ptFFF);
    def->label = L("Default extrusion width");
    def->category = OptionCategory::width;
    def->invalidates_step = posSlice;
    def->tooltip = L("This is the DEFAULT extrusion width. It's ONLY used to REPLACE 0-width fields. It's useless when all other width fields have a value."
        "\nSet this to a non-zero value to allow a manual extrusion width. "
        "If left to zero, Slic3r derives extrusion widths from the nozzle diameter "
        "(see the tooltips for perimeter extrusion width, infill extrusion width etc). "
        "If expressed as percentage (for example: 105%), it will be computed over nozzle diameter."
        "\nYou can set either 'Spacing', or 'Width'; the other will be calculated, using the perimeter 'Overlap' percentages and default layer height.");
    def->sidetext = L("mm or %");
    def->ratio_over = "nozzle_diameter";
    def->min = 0;
    def->max = 1000;
    def->max_literal = { 10, true };
    def->precision = 6;
    def->can_phony = true;
    def->mode = comExpert | comPrusa;
    def->set_default_value((new ConfigOptionFloatOrPercent(0, false))->set_phony(true));

    def = definition.add("extrusion_spacing", coFloatOrPercent, ptFFF);
    def->label = L("Default extrusion spacing");
    def->category = OptionCategory::width;
    def->invalidates_step = posSlice;
    def->tooltip = L("This is the DEFAULT extrusion spacing. It's convert to a width and this width can be used to REPLACE 0-width fields. It's useless when all  width fields have a value."
        "Like Default extrusion width but spacing is the distance between two lines (as they overlap a bit, it's not the same)."
                "\nYou can set either 'Spacing', or 'Width'; the other will be calculated, using the perimeter 'Overlap' percentages and default layer height.");
    def->sidetext = L("mm or %");
    def->ratio_over = "nozzle_diameter";
    def->min = 0;
    def->max = 1000;
    def->max_literal = { 10, true };
    def->precision = 6;
    def->can_phony = true;
    def->mode = comAdvancedE | comSuSi;
    def->set_default_value(new ConfigOptionFloatOrPercent(0, false));

#if 0
    //not used anymore, to remove !! @DEPRECATED (replaces by default_fan_speed)
    def = definition.add("fan_always_on", coBools, ptFFF);
    def->label = L("Keep fan always on");
    def->category = OptionCategory::cooling;
    def->tooltip = L("If this is enabled, fan will continuously run at base speed if no other setting overrides that speed."
                " Useful for PLA, harmful for ABS.");
    def->mode = comSimpleAE | comPrusa;
    def->is_vector_extruder = true;
    def->set_default_value(new ConfigOptionBools{ true });
#endif

    def = definition.add("fan_below_layer_time", coFloats, ptFFF);
    def->label = L("Enable fan if layer print time is below");
    def->category = OptionCategory::cooling;
    def->invalidates_step = psGCodeExport;
    def->tooltip = L("If layer print time is estimated below this number of seconds, fan will be enabled "
                "and its speed will be calculated by interpolating the default and maximum speeds."
                "\nSet zero to disable.");
    def->sidetext = L("approximate seconds");
    def->min = 0;
    def->max = 1000;
    def->mode = comExpert | comPrusa;
    def->is_vector_extruder = true;
    def->set_default_value(new ConfigOptionFloats { 60 });

    def = definition.add("filament_colour", coStrings, ptFFF);
    def->label = L("Color");
    def->full_label = L("Filament color");
    def->category = OptionCategory::filament;
    def->invalidates_step = psGCodeExport;
    def->tooltip = L("This is only used in the Slic3r interface as a visual help.");
    def->gui_type = ConfigOptionDef::GUIType::color;
    def->mode = comAdvancedE | comPrusa;
    def->is_vector_extruder = true;
    def->set_default_value(new ConfigOptionStrings{ "#29B2B2" });

    def = definition.add("filament_custom_variables", coStrings, ptFFF);
    def->label = L("Custom variables");
    def->full_label = L("Custom Filament variables");
    def->category = OptionCategory::filament;
    def->invalidates_step = psGCodeExport;
    def->tooltip = L("You can add data accessible to custom-gcode macros."
        "\nEach line can define one variable."
        "\nThe format is 'variable_name=value'. The variable name should only have [a-zA-Z0-9] characters or '_'."
        "\nA value that can be parsed as a int or float will be available as a numeric value."
        "\nA value that is enclosed by double-quotes will be available as a string (without the quotes)"
        "\nA value that only takes values as 'true' or 'false' will be a boolean)"
        "\nEvery other value will be parsed as a string as-is."
        "\nThese variables will be available as an array in the custom gcode (one item per extruder), don't forget to use them with the {current_extruder} index to get the current value."
        " If a filament has a typo on the variable that change its type, then the parser will convert everything to strings."
        "\nAdvice: before using a variable, it's safer to use the function 'default_XXX(variable_name, default_value)'"
        " (enclosed in bracket as it's a script) in case it's not set. You can replace XXX by 'int' 'bool' 'double' 'string'.");
    def->multiline = true;
    def->full_width = true;
    def->height = 13;
    def->mode = comAdvancedE | comSuSi;
    def->is_vector_extruder = true;
    def->set_default_value(new ConfigOptionStrings{ "" });

    def = definition.add("filament_fill_top_flow_ratio", coPercents, ptFFF);
    def->label = L("Top fill");
    def->full_label = L("Top fill flow ratio");
    def->sidetext = L("%");
    def->category = OptionCategory::width;
    def->invalidates_step = psGCodeExport;
    def->tooltip = L("You can increase this to over-extrude on the top layer if there is not enough plastic to make a good fill."
                    "\nThis setting multiply the percentage available in the print setting."
                    " You should only add the little percentage difference that this filament has versus your main one.");
    def->min = 0;
    def->mode = comExpert | comSuSi;
    def->set_default_value(new ConfigOptionPercents{100});

    def = definition.add("filament_first_layer_flow_ratio", coPercents, ptFFF);
    def->label = L("First layer");
    def->full_label = L("First layer flow ratio");
    def->sidetext = L("%");
    def->category = OptionCategory::width;
    def->invalidates_step = psGCodeExport;
    def->tooltip = L("You can increase this to over/under-extrude on the first layer if there is not enough / too many plastic because your bed isn't levelled / flat."
                    "\nThis setting multiply the percentage available in the print setting."
                    " You should only add the little percentage difference that this filament has versus your main one.");
    def->min = 0;
    def->mode = comExpert | comSuSi;
    def->set_default_value(new ConfigOptionPercents{100});

    def = definition.add("filament_notes", coStrings, ptFFF);
    def->label = L("Filament notes");
    def->category = OptionCategory::notes;
    def->invalidates_step = psGCodeExport;
    def->tooltip = L("You can put your notes regarding the filament here.");
    def->multiline = true;
    def->full_width = true;
    def->height = 13;
    def->mode = comAdvancedE | comPrusa;
    def->is_vector_extruder = true;
    def->set_default_value(new ConfigOptionStrings { "" });

    def = definition.add("filament_max_speed", coFloats, ptFFF);
    def->label = L("Max speed");
    def->category = OptionCategory::filament;
    def->invalidates_step = psSkirtBrim;
    def->tooltip = L("Maximum speed allowed for this filament. Limits the maximum "
        "speed of a print to the minimum of the print speed and the filament speed. "
        "Set zero for no limit.");
    def->sidetext = L("mm/s");
    def->min = 0;
    def->mode = comAdvancedE | comSuSi;
    def->is_vector_extruder = true;
    def->set_default_value(new ConfigOptionFloats{ 0. });

    def = definition.add("filament_max_volumetric_speed", coFloats, ptFFF);
    def->label = L("Max volumetric speed");
    def->category = OptionCategory::filament;
    def->invalidates_step = psSkirtBrim;
    def->tooltip = L("Maximum volumetric speed allowed for this filament. Limits the maximum volumetric "
        "speed of a print to the minimum of print and filament volumetric speed. "
        "Set zero for no limit.");
    def->sidetext = L("mm³/s");
    def->min = 0;
    def->mode = comAdvancedE | comPrusa;
    def->is_vector_extruder = true;
    def->set_default_value(new ConfigOptionFloats{ 0. });

    def = definition.add("filament_max_wipe_tower_speed", coFloats, ptFFF);
    def->label = L("Max speed on the wipe tower");
    def->invalidates_step = psSkirtBrim;
    def->tooltip = L("This setting is used to set the maximum speed when extruding inside the wipe tower (use M220)."
        " In %, set 0 to disable and use the Filament type instead."
        "\nIf disabled, these filament types will have a default value of:"
        "\n - PVA: 80% to 60%"
        "\n - SCAFF: 35%"
        "\n - FLEX: 35%"
        "\n - OTHERS: 100%"
        "\nNote that the wipe tower reset the speed at 100% for the unretract in any case." // TODO: "reset" -> "resets"?
        "\nIf using marlin, M220 B/R is used to save the speed override before the wipe tower print.");
    def->sidetext = L("%");
    def->min = 0;
    def->max = 400;
    def->mode = comExpert | comSuSi;
    def->is_vector_extruder = true;
    def->set_default_value(new ConfigOptionFloats { 0 });

    def = definition.add("filament_loading_speed", coFloats, ptFFF);
    def->label = L("Loading speed");
    def->invalidates_step = psSkirtBrim;
    def->tooltip = L("Speed used for loading the filament on the wipe tower. ");
    def->sidetext = L("mm/s");
    def->min = 0;
    def->mode = comExpert | comPrusa;
    def->is_vector_extruder = true;
    def->set_default_value(new ConfigOptionFloats { 28. });

    //skinnydip section starts
    def = definition.add("filament_enable_toolchange_temp", coBools, ptFFF);
    def->label = L("Toolchange temperature enabled");
    def->invalidates_step = psSkirtBrim;
    def->tooltip = L("Determines whether toolchange temperatures will be applied");
    def->mode = comAdvancedE | comSuSi;
    def->is_vector_extruder = true;
    def->set_default_value(new ConfigOptionBools { false });

    def = definition.add("filament_use_fast_skinnydip", coBools, ptFFF);
    def->label = L("Fast mode");
    def->invalidates_step = psSkirtBrim;
    def->tooltip = L("Experimental: drops nozzle temperature during cooling moves instead of prior to extraction to reduce wait time.");
    def->mode = comExpert | comSuSi;
    def->is_vector_extruder = true;
    def->set_default_value(new ConfigOptionBools { false });

    def = definition.add("filament_enable_toolchange_part_fan", coBools, ptFFF);
    def->label = L("Use part fan to cool hotend");
    def->invalidates_step = psSkirtBrim;
    def->tooltip = L("Experimental setting.  May enable the hotend to cool down faster during toolchanges");
    def->mode = comExpert | comSuSi;
    def->is_vector_extruder = true;
    def->set_default_value(new ConfigOptionBools { false });

    def = definition.add("filament_toolchange_part_fan_speed", coInts, ptFFF);
    def->label = L("Toolchange part fan speed");
    def->invalidates_step = psSkirtBrim;
    def->tooltip = L("Experimental setting.  Fan speeds that are too high can clash with the hotend's PID routine.");
    def->sidetext = L("%");
    def->min = 0;
    def->max = 100;
    def->mode = comExpert | comSuSi;
    def->is_vector_extruder = true;
    def->set_default_value(new ConfigOptionInts { 50 });

    def = definition.add("filament_use_skinnydip", coBools, ptFFF);
    def->label = L("Enable Skinnydip string reduction");
    def->invalidates_step = psSkirtBrim;
    def->tooltip = L("Skinnydip performs a secondary dip into the meltzone to burn off fine strings of filament");
    def->mode = comAdvancedE | comSuSi;
    def->is_vector_extruder = true;
    def->set_default_value(new ConfigOptionBools { false });

    def = definition.add("filament_melt_zone_pause", coInts, ptFFF);
    def->label = L("Pause in melt zone");
    def->invalidates_step = psSkirtBrim;
    def->tooltip = L("Stay in melt zone for this amount of time before extracting the filament.  Not usually necessary.");
    def->sidetext = L("milliseconds");
    def->min = 0;
    def->mode = comExpert | comSuSi;
    def->is_vector_extruder = true;
    def->set_default_value(new ConfigOptionInts { 0 });

    def = definition.add("filament_cooling_zone_pause", coInts, ptFFF);
    def->label = L("Pause before extraction ");
    def->invalidates_step = psSkirtBrim;
    def->tooltip = L("Can be useful to avoid bondtech gears deforming hot tips, but not ordinarily needed");
    def->sidetext = L("milliseconds");
    def->min = 0;
    def->mode = comExpert | comSuSi;
    def->is_vector_extruder = true;
    def->set_default_value(new ConfigOptionInts { 0 });

    def = definition.add("filament_dip_insertion_speed", coFloats, ptFFF);
    def->label = L("Speed to move into melt zone");
    def->invalidates_step = psSkirtBrim;
    def->tooltip = L("usually not necessary to change this");
    def->sidetext = L("mm/sec");
    def->min = 0;
    def->mode = comExpert | comSuSi;
    def->is_vector_extruder = true;
    def->set_default_value(new ConfigOptionFloats { 33. });

    def = definition.add("filament_dip_extraction_speed", coFloats, ptFFF);
    def->label = L("Speed to extract from melt zone");
    def->invalidates_step = psSkirtBrim;
    def->tooltip = L("usually not necessary to change this");
    def->sidetext = L("mm/sec");
    def->min = 0;
    def->mode = comExpert | comSuSi;
    def->is_vector_extruder = true;
    def->set_default_value(new ConfigOptionFloats { 70. });

    def = definition.add("filament_toolchange_temp", coInts, ptFFF);
    def->label = L("Toolchange temperature");
    def->invalidates_step = psSkirtBrim;
    def->tooltip = L("To further reduce stringing, it can be helpful to set a lower temperature just prior to extracting filament from the hotend.");
    def->sidetext = L("°C");
    def->min = 0;
    def->mode = comAdvancedE | comSuSi;
    def->is_vector_extruder = true;
    def->set_default_value(new ConfigOptionInts { 200 });

    def = definition.add("filament_skinnydip_distance", coFloats, ptFFF);
    def->label = L("Insertion distance");
    def->invalidates_step = psSkirtBrim;
    def->tooltip = L("For stock extruders, usually 40-42mm.  For bondtech extruder upgrade, usually 30-32mm.  Start with a low value and gradually increase it until strings are gone.  If there are blobs on your wipe tower, your value is too high.");
    def->sidetext = L("mm");
    def->min = 0;
    def->mode = comAdvancedE | comSuSi;
    def->is_vector_extruder = true;
    def->set_default_value(new ConfigOptionFloats { 31. });
    //skinnydip section ends

    def = definition.add("filament_loading_speed_start", coFloats, ptFFF);
    def->label = L("Loading speed at the start");
    def->invalidates_step = psSkirtBrim;
    def->tooltip = L("Speed used at the very beginning of loading phase. ");
    def->sidetext = L("mm/s");
    def->min = 0;
    def->mode = comExpert | comPrusa;
    def->is_vector_extruder = true;
    def->set_default_value(new ConfigOptionFloats { 3. });

    def = definition.add("filament_unloading_speed", coFloats, ptFFF);
    def->label = L("Unloading speed");
    def->invalidates_step = psSkirtBrim;
    def->tooltip = L("Speed used for unloading the filament on the wipe tower (does not affect "
                      " initial part of unloading just after ramming). ");
    def->sidetext = L("mm/s");
    def->min = 0;
    def->mode = comExpert | comPrusa;
    def->is_vector_extruder = true;
    def->set_default_value(new ConfigOptionFloats { 90. });

    def = definition.add("filament_unloading_speed_start", coFloats, ptFFF);
    def->label = L("Unloading speed at the start");
    def->invalidates_step = psSkirtBrim;
    def->tooltip = L("Speed used for unloading the tip of the filament immediately after ramming. ");
    def->sidetext = L("mm/s");
    def->min = 0;
    def->mode = comExpert | comPrusa;
    def->is_vector_extruder = true;
    def->set_default_value(new ConfigOptionFloats { 100. });

    def = definition.add("filament_toolchange_delay", coFloats, ptFFF);
    def->label = L("Delay after unloading");
    def->invalidates_step = psSkirtBrim;
    def->tooltip = L("Time to wait after the filament is unloaded. "
                   "May help to get reliable toolchanges with flexible materials "
                   "that may need more time to shrink to original dimensions. ");
    def->sidetext = L("s");
    def->min = 0;
    def->mode = comExpert | comPrusa;
    def->is_vector_extruder = true;
    def->set_default_value(new ConfigOptionFloats { 0. });

    def = definition.add("filament_cooling_moves", coInts, ptFFF);
    def->label = L("Number of cooling moves");
    def->invalidates_step = psSkirtBrim;
    def->tooltip = L("Filament is cooled by being moved back and forth in the "
                   "cooling tubes. Specify desired number of these moves.");
    def->max = 0;
    def->max = 20;
    def->mode = comExpert | comPrusa;
    def->is_vector_extruder = true;
    def->set_default_value(new ConfigOptionInts { 4 });

    def = definition.add("filament_cooling_initial_speed", coFloats, ptFFF);
    def->label = L("Speed of the first cooling move");
    def->invalidates_step = psSkirtBrim;
    def->tooltip = L("Cooling moves are gradually accelerated, starting at this speed. ");
    def->sidetext = L("mm/s");
    def->min = 0;
    def->mode = comExpert | comPrusa;
    def->is_vector_extruder = true;
    def->set_default_value(new ConfigOptionFloats { 2.2 });

    def = definition.add("filament_minimal_purge_on_wipe_tower", coFloats, ptFFF);
    def->label = L("Minimal purge on wipe tower");
    def->invalidates_step = psSkirtBrim;
    def->tooltip = L("After a tool change, the exact position of the newly loaded filament inside "
                     "the nozzle may not be known, and the filament pressure is likely not yet stable. "
                     "Before purging the print head into an infill or a sacrificial object, Slic3r will always prime "
                     "this amount of material into the wipe tower to produce successive infill or sacrificial object extrusions reliably.");
    def->sidetext = L("mm³");
    def->min = 0;
    def->mode = comExpert | comPrusa;
    def->is_vector_extruder = true;
    def->set_default_value(new ConfigOptionFloats { 15. });

    def = definition.add("filament_cooling_final_speed", coFloats, ptFFF);
    def->label = L("Speed of the last cooling move");
    def->invalidates_step = psSkirtBrim;
    def->tooltip = L("Cooling moves are gradually accelerated towards this speed. ");
    def->sidetext = L("mm/s");
    def->min = 0;
    def->mode = comExpert | comPrusa;
    def->is_vector_extruder = true;
    def->set_default_value(new ConfigOptionFloats { 3.4 });

    def = definition.add("filament_load_time", coFloats, ptFFF);
    def->label = L("Filament load time");
    def->invalidates_step = psGCodeExport;
    def->tooltip = L("Time for the printer firmware (or the Multi Material Unit 2.0) to load a new filament during a tool change (when executing the T code). This time is added to the total print time by the G-code time estimator.");
    def->sidetext = L("s");
    def->min = 0;
    def->mode = comExpert | comPrusa;
    def->is_vector_extruder = true;
    def->set_default_value(new ConfigOptionFloats { 0.0 });

    def = definition.add("filament_pressure_advance", coFloats, ptFFF);
    def->label = L("Pressure advance");
    def->tooltip = L("Default pressure advance value (Linear advance factor for Marlin)."
           " If enabled, the gcode will emit a pressure advance value for this filament."
           "\nWith reprap and sprinter, 'M572 Dx Sx' is used."
           "\nWith klipper, 'SET_PRESSURE_ADVANCE ADVANCE=x EXTRUDER=x' is used."
           "\nWith other firmware 'M900 Kx' is used.");
    def->category = OptionCategory::filament;
    def->invalidates_step = psGCodeExport;
    def->min = 0;
    def->can_be_disabled = true;
    def->mode = comAdvancedE | comSuSi;
    def->is_vector_extruder = true;
    def->set_default_value(disable_default_option(new ConfigOptionFloats({0.02})));
    def->aliases = {"filament_default_pa"};

    def = definition.add("filament_bridge_pa", coFloatsOrPercents, ptFFF);
    def->label = L("Bridge");
    def->category = OptionCategory::filament;
    def->tooltip = L("Pressure advance for bridge sections. Can be a % over default pa");
    def->mode = comExpert | comSuSi;
    def->ratio_over = "filament_pressure_advance";
    def->is_vector_extruder = true;
    def->set_default_value(new ConfigOptionFloatsOrPercents{ FloatOrPercent{100,true} });

    def = definition.add("filament_bridge_internal_pa", coFloatsOrPercents, ptFFF);
    def->label = L("Internal bridge");
    def->category = OptionCategory::filament;
    def->tooltip = L("Pressure advance for internal bridge sections. Can be a % over default pa");
    def->mode = comExpert | comSuSi;
    def->ratio_over = "filament_pressure_advance";
    def->is_vector_extruder = true;
    def->set_default_value(new ConfigOptionFloatsOrPercents{ FloatOrPercent{100,true} });

    def = definition.add("filament_brim_pa", coFloatsOrPercents, ptFFF);
    def->label = L("Brim");
    def->category = OptionCategory::filament;
    def->tooltip = L("Pressure advance for brim. Can be a % over support pa");
    def->mode = comExpert | comSuSi;
    def->ratio_over = "filament_support_material_pa";
    def->is_vector_extruder = true;
    def->set_default_value(new ConfigOptionFloatsOrPercents{ FloatOrPercent{100,true} });

    def = definition.add("filament_external_perimeter_pa", coFloatsOrPercents, ptFFF);
    def->label = L("External perimeter");
    def->category = OptionCategory::filament;
    def->tooltip = L("Pressure advance for external perimeter. Can be a % over support pa");
    def->mode = comExpert | comSuSi;
    def->ratio_over = "filament_support_material_pa";
    def->is_vector_extruder = true;
    def->set_default_value(new ConfigOptionFloatsOrPercents{ FloatOrPercent{100,true} });

    def = definition.add("filament_first_layer_pa", coFloatsOrPercents, ptFFF);
    def->label = L("First layer");
    def->category = OptionCategory::filament;
    def->tooltip = L("Pressure advance for first layer sections. If %, it's a % over the current feature");
    def->mode = comExpert | comSuSi;
    def->ratio_over = "depends";
    def->is_vector_extruder = true;
    def->set_default_value(new ConfigOptionFloatsOrPercents{ FloatOrPercent{100,true} });

    def = definition.add("filament_first_layer_pa_over_raft", coFloatsOrPercents, ptFFF);
    def->label = L("Over raft");
    def->category = OptionCategory::filament;
    def->tooltip = L("Pressure advance for first layer sections over raft . If %, it's a % over the current feature");
    def->mode = comExpert | comSuSi;
    def->ratio_over = "depends";
    def->is_vector_extruder = true;
    def->set_default_value(new ConfigOptionFloatsOrPercents{ FloatOrPercent{100,true} });

    def = definition.add("filament_gap_fill_pa", coFloatsOrPercents, ptFFF);
    def->label = L("Gap fill");
    def->category = OptionCategory::filament;
    def->tooltip = L("Pressure advance for gap fill sections. Can be a % over perimeter pa");
    def->mode = comExpert | comSuSi;
    def->ratio_over = "filament_perimeter_pa";
    def->is_vector_extruder = true;
    def->set_default_value(new ConfigOptionFloatsOrPercents{ FloatOrPercent{100,true} });

    def = definition.add("filament_infill_pa", coFloatsOrPercents, ptFFF);
    def->label = L("Infill");
    def->category = OptionCategory::filament;
    def->tooltip = L("Pressure advance for infill sections. Can be a % over solid infill pa");
    def->mode = comExpert | comSuSi;
    def->ratio_over = "filament_solid_infill_pa";
    def->is_vector_extruder = true;
    def->set_default_value(new ConfigOptionFloatsOrPercents{ FloatOrPercent{100,true} });

    def = definition.add("filament_ironing_pa", coFloatsOrPercents, ptFFF);
    def->label = L("Ironing");
    def->category = OptionCategory::filament;
    def->tooltip = L("Pressure advance for ironing sections. Can be a % over top solid infill pa");
    def->mode = comExpert | comSuSi;
    def->ratio_over = "filament_top_solid_infill_pa";
    def->is_vector_extruder = true;
    def->set_default_value(new ConfigOptionFloatsOrPercents{ FloatOrPercent{100,true} });

    def = definition.add("filament_overhangs_pa", coFloatsOrPercents, ptFFF);
    def->label = L("Overhangs");
    def->category = OptionCategory::filament;
    def->tooltip = L("Pressure advance for overhang sections. Can be a % over bridge pa");
    def->mode = comExpert | comSuSi;
    def->ratio_over = "filament_bridge_pa";
    def->is_vector_extruder = true;
    def->set_default_value(new ConfigOptionFloatsOrPercents{ FloatOrPercent{100,true} });

    def = definition.add("filament_perimeter_pa", coFloatsOrPercents, ptFFF);
    def->label = L("Perimeters");
    def->category = OptionCategory::filament;
    def->tooltip = L("Pressure advance for perimeter sections. Can be a % over default pa");
    def->mode = comExpert | comSuSi;
    def->ratio_over = "filament_pressure_advance";
    def->is_vector_extruder = true;
    def->set_default_value(new ConfigOptionFloatsOrPercents{ FloatOrPercent{100,true} });

    def = definition.add("filament_solid_infill_pa", coFloatsOrPercents, ptFFF);
    def->label = L("Solid infill");
    def->category = OptionCategory::filament;
    def->tooltip = L("Pressure advance for solid infill sections. Can be a % over default pa");
    def->mode = comExpert | comSuSi;
    def->ratio_over = "filament_pressure_advance";
    def->is_vector_extruder = true;
    def->set_default_value(new ConfigOptionFloatsOrPercents{ FloatOrPercent{100,true} });

    def = definition.add("filament_support_material_pa", coFloatsOrPercents, ptFFF);
    def->label = L("Support");
    def->category = OptionCategory::filament;
    def->tooltip = L("Pressure advance for support sections. Can be a % over default pa");
    def->mode = comExpert | comSuSi;
    def->ratio_over = "filament_pressure_advance";
    def->is_vector_extruder = true;
    def->set_default_value(new ConfigOptionFloatsOrPercents{ FloatOrPercent{100,true} });

    def = definition.add("filament_support_material_interface_pa", coFloatsOrPercents, ptFFF);
    def->label = L("Support interface");
    def->category = OptionCategory::filament;
    def->tooltip = L("Pressure advance for support interface sections. Can be a % over support pa");
    def->mode = comExpert | comSuSi;
    def->ratio_over = "filament_support_material_pa";
    def->is_vector_extruder = true;
    def->set_default_value(new ConfigOptionFloatsOrPercents{ FloatOrPercent{100,true} });

    def = definition.add("filament_thin_walls_pa", coFloatsOrPercents, ptFFF);
    def->label = L("Thin walls");
    def->category = OptionCategory::filament;
    def->tooltip = L("Pressure advance for thin wall sections. Can be a % over external perimeter pa");
    def->mode = comExpert | comSuSi;
    def->ratio_over = "filament_external_perimeter_pa";
    def->is_vector_extruder = true;
    def->set_default_value(new ConfigOptionFloatsOrPercents{ FloatOrPercent{100,true} });

    def = definition.add("filament_top_solid_infill_pa", coFloatsOrPercents, ptFFF);
    def->label = L("Top solid infill");
    def->category = OptionCategory::filament;
    def->tooltip = L("Pressure advance for top solid infill sections. Can be a % over solid infill pa");
    def->mode = comExpert | comSuSi;
    def->ratio_over = "filament_solid_infill_pa";
    def->is_vector_extruder = true;
    def->set_default_value(new ConfigOptionFloatsOrPercents{ FloatOrPercent{100,true} });

    def = definition.add("filament_travel_pa", coFloatsOrPercents, ptFFF);
    def->label = L("Travel");
    def->category = OptionCategory::filament;
    def->tooltip = L("Pressure advance for travel sections, may help retraction and unretraction."
            " Can be a % over default pa."
            "\nSet -1 to let the previous pa continue in the travel.");
    def->mode = comExpert | comSuSi;
    def->ratio_over = "filament_pressure_advance";
    def->is_vector_extruder = true;
    def->can_be_disabled = true;
    def->set_default_value(disable_default_option(new ConfigOptionFloatsOrPercents({FloatOrPercent{0, false}})));

    def = definition.add("filament_ramming_parameters", coStrings, ptFFF);
    def->label = L("Ramming parameters");
    def->invalidates_step = psSkirtBrim;
    def->tooltip = L("This string is edited by RammingDialog and contains ramming specific parameters.");
    def->mode = comExpert | comPrusa;
    def->is_vector_extruder = true;
    def->set_default_value(new ConfigOptionStrings { "120 100 6.6 6.8 7.2 7.6 7.9 8.2 8.7 9.4 9.9 10.0|"
       " 0.05 6.6 0.45 6.8 0.95 7.8 1.45 8.3 1.95 9.7 2.45 10 2.95 7.6 3.45 7.6 3.95 7.6 4.45 7.6 4.95 7.6" });

    def = definition.add("filament_unload_time", coFloats, ptFFF);
    def->label = L("Filament unload time");
    def->invalidates_step = psGCodeExport;
    def->tooltip = L("Time for the printer firmware (or the Multi Material Unit 2.0) to unload a filament during a tool change (when executing the T code). This time is added to the total print time by the G-code time estimator.");
    def->sidetext = L("s");
    def->min = 0;
    def->mode = comExpert | comPrusa;
    def->is_vector_extruder = true;
    def->set_default_value(new ConfigOptionFloats { 0.0 });

    def = definition.add("filament_multitool_ramming", coBools, ptFFF);
    def->label = L("Enable ramming for multitool setups");
    def->invalidates_step = psSkirtBrim;
    def->tooltip = L("Perform ramming when using multitool printer (i.e. when the 'Single Extruder Multimaterial' in Printer Settings is unchecked). "
                     "When checked, a small amount of filament is rapidly extruded on the wipe tower just before the toolchange. "
                     "This option is only used when the wipe tower is enabled.");
    def->mode = comExpert | comPrusa;
    def->set_default_value(new ConfigOptionBools { false });

    def = definition.add("filament_multitool_ramming_volume", coFloats, ptFFF);
    def->label = L("Multitool ramming volume");
    def->invalidates_step = psSkirtBrim;
    def->tooltip = L("The volume to be rammed before the toolchange.");
    def->sidetext = L("mm³");
    def->min = 0;
    def->mode = comExpert | comPrusa;
    def->set_default_value(new ConfigOptionFloats { 10. });

    def = definition.add("filament_multitool_ramming_flow", coFloats, ptFFF);
    def->label = L("Multitool ramming flow");
    def->invalidates_step = psSkirtBrim;
    def->tooltip = L("Flow used for ramming the filament before the toolchange.");
    def->sidetext = L("mm³/s");
    def->min = 0;
    def->mode = comExpert | comPrusa;
    def->set_default_value(new ConfigOptionFloats { 10. });

    def = definition.add("filament_diameter", coFloats, ptFFF);
    def->label = L("Diameter");
    def->invalidates_step = psGCodeExport;
    def->tooltip = L("Enter your filament diameter here. Good precision is required, so use a caliper "
                   "and do multiple measurements along the filament, then compute the average.");
    def->sidetext = L("mm");
    def->min = 0;
    def->mode = comAdvancedE | comPrusa;
    def->is_vector_extruder = true;
    def->set_default_value(new ConfigOptionFloats{ 1.75 });

    def = definition.add("filament_shrink", coPercents, ptFFF);
    def->label = L("Shrinkage");
    def->invalidates_step = posSlice;
    def->tooltip = L("Enter the shrinkage percentage that the filament will get after cooling (94% if you measure 94mm instead of 100mm)."
        " The part will be scaled in xy to compensate."
        " Only the filament used for the perimeter is taken into account."
        "\nBe sure to allow enough space between objects, as this compensation is done after the checks.");
    def->sidetext = L("%");
    def->ratio_over = "";
    def->min = 10;
    def->mode = comExpert | comSuSi;
    def->is_vector_extruder = true;
    def->set_default_value(new ConfigOptionPercents{ 100 });

    def = definition.add("filament_max_overlap", coPercents, ptFFF);
    def->label = L("Max line overlap");
    def->invalidates_step = posSlice;
    def->tooltip = L("This setting will ensure that all 'overlap' are not higher than this value."
        " This is useful for filaments that are too viscous, as the line can't flow under the previous one."
        "\nNote: top solid infill lines are excluded, to prevent visual defects.");
    def->sidetext = L("%");
    def->ratio_over = "";
    def->min = 0;
    def->max = 100;
    def->mode = comExpert | comSuSi;
    def->is_vector_extruder = true;
    def->set_default_value(new ConfigOptionPercents{ 100 });

    def = definition.add("filament_density", coFloats, ptFFF);
    def->label = L("Density");
    def->category = OptionCategory::filament;
    def->invalidates_step = psGCodeExport;
    def->tooltip = L("Enter your filament density here. This is only for statistical information. "
                   "A decent way is to weigh a known length of filament and compute the ratio "
                   "of the length to volume. Better is to calculate the volume directly through displacement.");
    def->sidetext = L("g/cm³");
    def->min = 0;
    def->mode = comAdvancedE | comPrusa;
    def->is_vector_extruder = true;
    def->set_default_value(new ConfigOptionFloats{ 0. });

    def = definition.add("filament_type", coStrings, ptFFF);
    def->label = L("Filament type");
    def->category = OptionCategory::filament;
    def->invalidates_step = psSkirtBrim;
    def->tooltip = L("The filament material type for use in custom G-codes.");
    def->gui_flags = "show_value";
    def->set_enum_values(ConfigOptionDef::GUIType::select_open, {
        "PLA", 
        "PET",
        "ABS",
        "ASA",
        "FLEX", 
        "HIPS",
        "EDGE",
        "NGEN",
        "PA",
        "NYLON",
        "PVA",
        "PC",
        "PP",
        "PEI",
        "PEEK",
        "PEKK",
        "POM",
        "PSU",
        "PVDF",
        "SCAFF"
    });
    def->mode = comAdvancedE | comPrusa;
    def->is_vector_extruder = true;
    def->set_default_value(new ConfigOptionStrings { "PLA" });

    def = definition.add("filament_soluble", coBools, ptFFF);
    def->label = L("Soluble material");
    def->category = OptionCategory::filament;
    def->invalidates_step = posSupportMaterial;
    def->tooltip = L("Soluble material is most likely used for a soluble support.");
    def->mode = comAdvancedE | comPrusa;
    def->is_vector_extruder = true;
    def->set_default_value(new ConfigOptionBools { false });

    def = definition.add("filament_cost", coFloats, ptFFF);
    def->label = L("Cost");
    def->full_label = L("Filament cost");
    def->category = OptionCategory::filament;
    def->invalidates_step = psGCodeExport;
    def->tooltip = L("Enter your filament cost per kg here. This is only for statistical information.");
    def->sidetext = L("money/kg");
    def->min = 0;
    def->is_vector_extruder = true;
    def->mode = comSimpleAE | comPrusa;
    def->set_default_value(new ConfigOptionFloats { 0. });

    def = definition.add("filament_spool_weight", coFloats, ptFFF);
    def->label = L("Spool weight");
    def->category = OptionCategory::filament;
    def->invalidates_step = psGCodeExport;
    def->tooltip = L("Enter weight of the empty filament spool. "
                     "One may weigh a partially consumed filament spool before printing and one may compare the measured weight "
                     "with the calculated weight of the filament with the spool to find out whether the amount "
                     "of filament on the spool is sufficient to finish the print.");
    def->sidetext = L("g");
    def->min = 0;
    def->is_vector_extruder = true;
    def->mode = comSimpleAE | comPrusa;
    def->set_default_value(new ConfigOptionFloats { 0. });

    def = definition.add("filament_settings_id", coStrings, ptFFF);
    def->mode = comNone | comPrusa; // note: hidden setting
    def->set_default_value(new ConfigOptionStrings { "" });
    def->cli = ConfigOptionDef::nocli;

    def = definition.add("filament_settings_modified", coBools, ptFFF);
    def->mode = comNone | comPrusa; // note: hidden setting
    def->set_default_value(new ConfigOptionBools({false}));
    def->cli = ConfigOptionDef::nocli;

    def = definition.add("filament_vendor", coString, ptFFF);
    def->mode = comNone | comPrusa; // note: hidden setting
    def->set_default_value(new ConfigOptionString(L("(Unknown)")));
    def->cli = ConfigOptionDef::nocli;

    def = definition.add("fill_aligned_z", coBool, ptFFF);
    def->label = L("Align sparse infill in z");
    def->category = OptionCategory::infill;
    def->invalidates_step = posInfill;
    def->tooltip = L("The voids of the pattern of sparse infill grows with the extrusion width, to keep the same percentage of fill."
                    " With this setting set to true, the algorithms will now only use the highest sparse infill width available to create the pattern."
                    " This way, the pattern can still be aligned even if the width is changing (from first layer width, from a modifier, from another extruder with different diameter)."
                    "\nExperimental: works only for infill that won't depends on the fill area. So for infill where it's not useful (Hilbert, Archimedean, Octagram, Scattered, Lightning), this setting is disabled."
                    "\n This setting is useful for rectilinear, monotonic, grid, triangle, star, cubic, gyroid, honeycomb, 3D honeycomb, adaptive cubic, support cubic patterns.");
    def->mode = comExpert | comSuSi;
    def->set_default_value(new ConfigOptionBool(true));

    def = definition.add("fill_angle", coFloat, ptFFF);
    def->label = L("Fill");
    def->full_label = L("Fill angle");
    def->category = OptionCategory::infill;
    def->invalidates_step = posInfill;
    def->tooltip = L("Default base angle for infill orientation. Cross-hatching will be applied to this. "
                   "Bridges will be infilled using the best direction Slic3r can detect, so this setting "
                   "does not affect them.");
    def->sidetext = L("°");
    def->min = 0;
    def->max = 360;
    def->mode = comAdvancedE | comPrusa;
    def->set_default_value(new ConfigOptionFloat(45));

    def             = definition.add("fill_angle_cross", coBool, ptFFF);
    def->label      = L("Alternate Fill Angle");
    def->category   = OptionCategory::infill;
    def->invalidates_step = posInfill;
    def->tooltip    = L("It's better for some infill like rectilinear to rotate 90° each layer. If this setting is deactivated, they won't do that anymore.");
    def->mode       = comAdvancedE | comSuSi;
    def->set_default_value(new ConfigOptionBool(true));

    def             = definition.add("fill_angle_follow_model", coBool, ptFFF);
    def->label      = L("Rotate with object");
    def->category   = OptionCategory::infill;
    def->invalidates_step = posInfill;
    def->tooltip    = L("If your object has a z-rotation, then the infill will also be rotated by this value.");
    def->mode       = comAdvancedE | comSuSi;
    def->set_default_value(new ConfigOptionBool(false));

    def = definition.add("fill_angle_increment", coFloat, ptFFF);
    def->label = L("Fill");
    def->full_label = L("Fill angle increment");
    def->category = OptionCategory::infill;
    def->invalidates_step = posInfill;
    def->tooltip = L("Add this angle each layer to the base angle for infill. "
                    "May be useful for art, or to be sure to hit every object's feature even with very low infill. "
                    "Still experimental, tell me what makes it useful, or the problems that arise using it.");
    def->sidetext = L("°");
    def->min = 0;
    def->max = 360;
    def->mode = comExpert | comSuSi;
    def->set_default_value(new ConfigOptionFloat(0));

    def             = definition.add("fill_angle_template", coFloats, ptFFF);
    def->label      = L("Fill angle template");
    def->full_label = L("Fill angle template");
    def->category   = OptionCategory::infill;
    def->invalidates_step = posInfill;
    def->tooltip    = L("This define the succetion of infill angle. When defined, it replaces the fill_angle"
        ", and there won't be any extra 90° for each layer added, but the fill_angle_increment will still be used."
        " The first layer start with the first angle. If a new pattern is used in a modifier"
        ", it will choose the layer angle from the pattern as if it has started from the first layer."
        "Empty this settings to disable and recover the old behavior.");
    def->sidetext   = L("°");
    def->min        = -360;
    def->max        = 360;
    def->full_width = true;
    def->mode       = comExpert | comSuSi;
    def->set_default_value(new ConfigOptionFloats(0.));

    def = definition.add("fill_density", coPercent, ptFFF);
    def->label = L("Fill density");
    def->category = OptionCategory::infill;
    def->invalidates_step = posPrepareInfill;
    def->tooltip = L("Density of internal infill, expressed in the range 0% - 100%."
        "\nSet 0 to remove any sparse infill."
        "\nNote that using a value of 100% won't change the type of infill from sparse to solid."
        " If you want only solid infill, you can set the 'Solid infill every X layers' (solid_infill_every_layers) to 1 instead.");
    def->sidetext = L("%");
    def->min = 0;
    def->max = 100;
    def->set_enum_values(ConfigOptionDef::GUIType::f_enum_open, {
        { "0", "0%" },
        { "4", "4%" },
        { "5.5", "5.5%" },
        { "7.5", "7.5%" },
        { "10", "10%" },
        { "13", "13%" },
        { "18", "18%" },
        { "23", "23%" },
        { "31", "31%" },
        { "42", "42%" },
        { "55", "55%" },
        { "75", "75%" },
        //{ "100", "100%" } // can still be entered, but not showing it may make people increase solid layer count instead (which is the proper way).
    });
    def->mode = comSimpleAE | comPrusa;
    def->set_default_value(new ConfigOptionPercent(18));

    def = definition.add("fill_pattern", coEnum, ptFFF);
    def->label = L("Pattern");
    def->full_label = L("Sparse fill pattern");
    def->category = OptionCategory::infill;
    def->invalidates_step = posInfill;
    def->tooltip = L("Fill pattern for general low-density infill."
        "\nIf you want an 'aligned' pattern, set 90° to the fill angle increment setting.");
    def->set_enum<InfillPattern>({
        { "rectilinear",        L("Rectilinear") },
        { "alignedrectilinear", L("Aligned Rectilinear") },
        { "monotonic",          L("Monotonic") },
        { "grid",               L("Grid") }, 
        { "triangles",          L("Triangles")},
        { "stars",              L("Stars")},
        { "cubic",              L("Cubic")},
        { "line",               L("Line")},
        { "concentric",         L("Concentric")},
        { "honeycomb",          L("Honeycomb")},
        { "3dhoneycomb",        L("3D Honeycomb")},
        { "gyroid",             L("Gyroid")},
        { "hilbertcurve",       L("Hilbert Curve")},
        { "archimedeanchords",  L("Archimedean Chords")},
        { "octagramspiral",     L("Octagram Spiral")},
        {"scatteredrectilinear",L("Scattered Rectilinear")},
        { "adaptivecubic",      L("Adaptive Cubic")},
        { "supportcubic",       L("Support Cubic")},
        { "lightning",          L("Lightning")}
    });
    def->mode = comSimpleAE | comPrusa;
    def->set_default_value( new ConfigOptionEnum<InfillPattern>(ipStars));

    def = definition.add("fill_top_flow_ratio", coPercent, ptFFF);
    def->label = L("Top fill");
    def->full_label = L("Top fill flow ratio");
    def->sidetext = L("%");
    def->category = OptionCategory::width;
    def->invalidates_step = posInfill;
    def->tooltip = L("You can increase this to over-extrude on the top layer if there is not enough plastic to make a good fill.");
    def->min = 0;
    def->mode = comExpert | comSuSi;
    def->set_default_value(new ConfigOptionPercent(100));

    def = definition.add("first_layer_flow_ratio", coPercent, ptFFF);
    def->label = L("First layer");
    def->full_label = L("First layer flow ratio");
    def->sidetext = L("%");
    def->category = OptionCategory::width;
    def->invalidates_step = psGCodeExport;
    def->tooltip = L("You can increase this to over-extrude on the first layer if there is not enough plastic because your bed isn't levelled."
                    "\nNote: DON'T USE THIS if your only problem is bed leveling, LEVEL YOUR BED!"
                    " Use this setting only as last resort after all calibrations failed.");
    def->min = 0;
    def->mode = comExpert | comSuSi;
    def->set_default_value(new ConfigOptionPercent(100));

    def = definition.add("first_layer_size_compensation", coFloat, ptFFF);
    def->label = L("First layer");
    def->full_label = L("XY First layer compensation");
    def->category = OptionCategory::slicing;
    def->invalidates_step = posSlice;
    def->tooltip = L("The first layer will be grown / shrunk in the XY plane by the configured value "
        "to compensate for the 1st layer squish aka an Elephant Foot effect. (should be negative = inwards = remove area)");
    def->sidetext = L("mm");
    def->mode = comAdvancedE | comSuSi | comPrusa; // just a rename & inverted of prusa 's elefant_foot
    def->set_default_value(new ConfigOptionFloat(0));

    def = definition.add("first_layer_size_compensation_layers", coInt, ptFFF);
    def->label = L("height in layers");
    def->full_label = L("XY First layer compensation height in layers");
    def->category = OptionCategory::slicing;
    def->invalidates_step = posSlice;
    def->tooltip = L("The number of layers on which the elephant foot compensation will be active. "
        "The first layer will be shrunk by the elephant foot compensation value, then "
        "the next layers will be gradually shrunk less, up to the layer indicated by this value.");
    def->sidetext = L("layers");
    def->min = 1;
    def->max = 30;
    def->mode = comAdvancedE | comSuSi;
    def->set_default_value(new ConfigOptionInt(1));

    def = definition.add("first_layer_size_compensation_no_collapse", coBool, ptFFF);
    def->label = L("No collapse");
    def->full_label = L("XY First layer compensation: no collapse");
    def->category = OptionCategory::slicing;
    def->invalidates_step = posSlice;
    def->tooltip = L("The compensations won't shrink thin areas below a threshold for the first layer(s)."
                    "\nThe layer(s) where this is activated depends on the 'first_layer_size_compensation_layers' setting.");
    def->mode = comAdvancedE | comSuSi;
    def->set_default_value(new ConfigOptionBool(true));

    def = definition.add("first_layer_strong_start", coPercent, ptFFF);
    def->label = L("First layer squished start point");
    def->full_label = L("First layer squished start point");
    def->sidetext = L("%");
    def->category = OptionCategory::width;
    def->invalidates_step = psGCodeExport;
    def->tooltip = L("When on the first layer, to be sure the start of an extrusion attach to the build plate, the extrude will first extrude on place before to start moving."
        " The amount extruded is deduced from the next extrusions (they will still extrude at 10%)."
        " 100% is equivalent of the amount extruded for an extrusion of 'nozzle diameter' width over the distance of 'nozzle diameter'."
    );
    def->min = 0;
    def->mode = comExpert | comSuSi;
    def->set_default_value(new ConfigOptionPercent(0));

    def = definition.add("fill_smooth_width", coFloatOrPercent, ptFFF);
    def->label = L("Width");
    def->full_label = L("Ironing width");
    def->category = OptionCategory::infill;
    def->invalidates_step = posInfill;
    def->tooltip = L("This is the width of the ironing pass, in a % of the top infill extrusion width, should not be more than 50%"
        " (two times more lines, 50% overlap). It's not necessary to go below 25% (four times more lines, 75% overlap). \nIf you have problems with your ironing process,"
        " don't forget to look at the flow->above bridge flow, as this setting should be set to min 110% to let you have enough plastic in the top layer."
        " A value too low will make your extruder eat the filament.");
    def->ratio_over = "top_infill_extrusion_width";
    def->min = 0;
    def->max_literal = { 1, true };
    def->mode = comExpert | comSuSi;
    def->sidetext = L("mm/%");
    def->set_default_value(new ConfigOptionFloatOrPercent(50, true));

    def = definition.add("fill_smooth_distribution", coPercent, ptFFF);
    def->label = L("Distribution");
    def->full_label = L("Ironing flow distribution");
    def->category = OptionCategory::infill;
    def->invalidates_step = posInfill;
    def->tooltip = L("This is the percentage of the flow that is used for the second ironing pass. Typical 10-20%. "
        "Should not be higher than 20%, unless you have your top extrusion width greatly superior to your nozzle width. "
        "A value too low and your extruder will eat the filament. A value too high and the first pass won't print well.");
    //def->min = 0;
    //def->max = 0.9;
    def->mode = comExpert | comSuSi;
    def->sidetext = L("%");
    def->set_default_value(new ConfigOptionPercent(10));

    def = definition.add("small_area_infill_flow_compensation_model", coGraph, ptFFF);
    def->label = L("Flow Compensation Model");
    def->category = OptionCategory::infill;
    def->invalidates_step = psGCodeExport;
    def->tooltip = L("Flow Compensation Model, used to adjust the flow for small solid infill "
                     "lines. The model is a graph of flow correction factors (between 0 and 1) per extrusion length (in mm)."
                     "\nThe first point length has to be 0mm. the last point need to have a flow correction of 1."
                    "\nIt's always disabled on the first layer, to not compromise adhesion.");
    def->mode = comExpert | comSuSi;
    def->can_be_disabled = true;
    def->set_default_value(disable_default_option(new ConfigOptionGraph(GraphData(0,10, GraphData::GraphType::SPLINE,
        {{0,0},{0.2,0.44},{0.4,0.61},{0.6,0.7},{0.8,0.76},{1.5,0.86},{2,0.89},{3,0.92},{5,0.95},{10,1}}
    ))));
    def->graph_settings = std::make_shared<GraphSettings>();
    def->graph_settings->title       = L("Flow Compensation Model");
    def->graph_settings->description = def->tooltip;
    def->graph_settings->x_label     = L("Length of an extrusion (mm)");
    def->graph_settings->y_label     = L("Flow correction (ratio between 0 and 1)");
    def->graph_settings->null_label  = L("No values");
    def->graph_settings->label_min_x = "";
    def->graph_settings->label_max_x = L("Maximum length");
    def->graph_settings->label_min_y = L("Minimum ratio");
    def->graph_settings->label_max_y = L("Maximum ratio");
    def->graph_settings->min_x       = 0;
    def->graph_settings->max_x       = 100;
    def->graph_settings->step_x      = 0.1;
    def->graph_settings->min_y       = 0;
    def->graph_settings->max_y       = 1;
    def->graph_settings->step_y      = 0.01;
    def->graph_settings->allowed_types = {GraphData::GraphType::LINEAR, GraphData::GraphType::SPLINE, GraphData::GraphType::SQUARE};

    def = definition.add("first_layer_acceleration", coFloatOrPercent, ptFFF);
    def->label = L("Max");
    def->full_label = L("First layer acceleration");
    def->category = OptionCategory::speed;
    def->invalidates_step = psGCodeExport;
    def->tooltip = L("This is the maximum acceleration your printer will use for first layer."
                "\nIf set to %, all accelerations will be reduced by that ratio."
                "\nSet zero to disable acceleration control for first layer.");
    def->sidetext = L("mm/s² or %");
    def->ratio_over = "depends";
    def->min = 0;
    def->max_literal = { -200, false };
    def->mode = comExpert | comPrusa;
    def->set_default_value(new ConfigOptionFloatOrPercent(0, false));

    def = definition.add("first_layer_acceleration_over_raft", coFloatOrPercent, ptFFF);
    def->label = L("First object layer over raft interface");
    def->category = OptionCategory::speed;
    def->invalidates_step = psGCodeExport;
    def->tooltip = L("This is the acceleration your printer will use for first layer of object above raft interface."
        "\nIf set to %, all accelerations will be reduced by that ratio."
        "\nSet zero to disable acceleration control for first layer of object above raft interface.");
    def->sidetext = L("mm/s²");
    def->min = 0;
    def->mode = comExpert | comPrusa;
    def->set_default_value(new ConfigOptionFloatOrPercent(0, false));

    def = definition.add("first_layer_bed_temperature", coInts, ptFFF);
    def->label = L("First layer");
    def->full_label = L("First layer bed temperature");
    def->category = OptionCategory::filament;
    def->invalidates_step = psGCodeExport;
    def->tooltip = L("Heated build plate temperature for the first layer. Set zero to disable "
                   "bed temperature control commands in the output."
                   "\nSet to 0 to prevent the slicer to do any first layer bed command.");
    def->sidetext = L("°C");
    def->max = 0;
    def->max = 300;
    def->is_vector_extruder = true;
    def->mode = comSimpleAE | comPrusa;
    def->set_default_value(new ConfigOptionInts { 0 });

    def = definition.add("first_layer_extrusion_width", coFloatOrPercent, ptFFF);
    def->label = L("First layer");
    def->full_label = L("First layer width");
    def->category = OptionCategory::width;
    def->invalidates_step = posSlice;
    def->tooltip = L("Set this to a non-zero value to set a manual extrusion width for first layer. "
        "You can use this to force fatter extrudates for better adhesion. If expressed "
        "as percentage (for example 140%) it will be computed over the nozzle diameter "
        "of the nozzle used for the type of extrusion. "
        "If set to zero, it will use the default extrusion width."
        "If disabled, nothing is changed compared to a normal layer."
        "\nYou can set either 'Spacing', or 'Width'; the other will be calculated, using the perimeter 'Overlap' percentages and default layer height.");
    def->sidetext = L("mm or %");
    def->ratio_over = "nozzle_diameter";
    def->min = 0;
    def->max = 1000;
    def->max_literal = { 10, true };
    def->precision = 6;
    def->can_phony = true;
    def->can_be_disabled = true;
    def->mode = comAdvancedE | comPrusa;
    def->set_default_value(enable_default_option(new ConfigOptionFloatOrPercent(140, true)));

    def = definition.add("first_layer_extrusion_spacing", coFloatOrPercent, ptFFF);
    def->label = L("First layer");
    def->full_label = L("First layer spacing");
    def->category = OptionCategory::width;
    def->invalidates_step = posSlice;
    def->tooltip = L("Like First layer width but spacing is the distance between two lines (as they overlap a bit, it's not the same)."
        "\nYou can set either 'Spacing', or 'Width'; the other will be calculated, using the perimeter 'Overlap' percentages and default layer height.");
    def->sidetext = L("mm or %");
    def->ratio_over = "nozzle_diameter";
    def->min = 0;
    def->max = 1000;
    def->max_literal = { 10, true };
    def->precision = 6;
    def->can_phony = true;
    def->mode = comAdvancedE | comSuSi;
    def->set_default_value((new ConfigOptionFloatOrPercent(0, false))->set_phony(true));
    

    def = definition.add("first_layer_infill_extrusion_width", coFloatOrPercent, ptFFF);
    def->label = L("First layer");
    def->full_label = L("First layer infill width");
    def->category = OptionCategory::width;
    def->invalidates_step = posInfill;
    def->tooltip = L("Set this to a non-zero value to set a manual extrusion width for first layer infill (sparse and solid). "
        "You can use this to force fatter extrudates for better adhesion. If expressed "
        "as percentage (for example 140%) it will be computed over the nozzle diameter "
        "of the nozzle used for the type of extrusion. "
        "If set to zero, it will use the default extrusion width."
        "If disabled, the first layer width is also used for first layer infills (if enabled)."
        "\nYou can set either 'Spacing', or 'Width'; the other will be calculated, using the perimeter 'Overlap' percentages and default layer height.");
    def->sidetext = L("mm or %");
    def->ratio_over = "nozzle_diameter";
    def->min = 0;
    def->max = 1000;
    def->max_literal = { 10, true };
    def->precision = 6;
    def->can_phony = true;
    def->can_be_disabled = true;
    def->mode = comAdvancedE | comSuSi;
    def->set_default_value(disable_default_option(new ConfigOptionFloatOrPercent(140, true)));

    def = definition.add("first_layer_infill_extrusion_spacing", coFloatOrPercent, ptFFF);
    def->label = L("First layer");
    def->full_label = L("First layer infill spacing");
    def->category = OptionCategory::width;
    def->invalidates_step = posInfill;
    def->tooltip = L("Like First layer infill width but spacing is the distance between two lines (as they overlap a bit, it's not the same)."
        "\nYou can set either 'Spacing', or 'Width'; the other will be calculated, using the perimeter 'Overlap' percentages and default layer height.");
    def->sidetext = L("mm or %");
    def->ratio_over = "nozzle_diameter";
    def->min = 0;
    def->max = 1000;
    def->max_literal = { 10, true };
    def->precision = 6;
    def->can_phony = true;
    def->mode = comAdvancedE | comSuSi;
    def->set_default_value((new ConfigOptionFloatOrPercent(0, false))->set_phony(true));

    def = definition.add("first_layer_height", coFloatOrPercent, ptFFF);
    def->label = L("First layer height");
    def->category = OptionCategory::slicing;
    def->invalidates_step = posSlice;
    def->tooltip = L("When printing with very low layer heights, you might still want to print a thicker "
                   "bottom layer to improve adhesion and tolerance for non perfect build plates. "
                   "This can be expressed as an absolute value or as a percentage (for example: 75%) "
                   "over the lowest nozzle diameter used in by the object.");
    def->sidetext = L("mm or %");
    def->ratio_over = "nozzle_diameter";
    def->min = 0;
    def->max_literal = { 20, false };
    def->mode = comAdvancedE | comPrusa;
    def->set_default_value(new ConfigOptionFloatOrPercent(75, true));

    def = definition.add("first_layer_speed", coFloatOrPercent, ptFFF);
    def->label = L("Max");
    def->full_label = L("Default first layer speed");
    def->category = OptionCategory::speed;
    def->invalidates_step = psGCodeExport;
    def->tooltip = L("If expressed as absolute value in mm/s, this speed will be applied as a maximum to all the print moves (but infill) of the first layer."
        "\nIf expressed as a percentage it will scale the current speed."
        "\nSet it at 100% to remove any first layer speed modification (but for infill).");
    def->sidetext = L("mm/s or %");
    def->ratio_over = "depends";
    def->min = 0;
    def->mode = comAdvancedE | comPrusa;
    def->set_default_value(new ConfigOptionFloatOrPercent(30, false));

    def = definition.add("first_layer_speed_over_raft", coFloatOrPercent, ptFFF);
    def->label = L("Max speed of object first layer over raft interface");
    def->category = OptionCategory::speed;
    def->invalidates_step = psGCodeExport;
    def->tooltip = L("If expressed as absolute value in mm/s, this speed will be usedf as a max over all the print moves "
        "of the first object layer above raft interface, regardless of their type."
        "\nIf expressed as a percentage it will scale the current speed (max 100%)."
        "\nSet it at 100% to remove this speed modification.");
    def->sidetext = L("mm/s or %");
    def->ratio_over = "depends";
    def->min = 0;
    def->mode = comAdvancedE | comPrusa;
    def->set_default_value(new ConfigOptionFloatOrPercent(30, false));
    
    def = definition.add("first_layer_infill_speed", coFloatOrPercent, ptFFF);
    def->label = L("Max infill");
    def->full_label = L("Infill max first layer speed");
    def->category = OptionCategory::speed;
    def->invalidates_step = psGCodeExport;
    def->tooltip = L("If expressed as absolute value in mm/s, this speed will be applied as a maximum for all infill print moves of the first layer."
                   "\nIf expressed as a percentage it will scale the current infill speed."
                   "\nSet it at 100% to remove any infill first layer speed modification."
                   "\nSet zero to disable (using first_layer_speed instead).");
    def->sidetext = L("mm/s or %");
    def->ratio_over = "depends";
    def->min = 0;
    def->mode = comExpert | comSuSi;
    def->set_default_value(new ConfigOptionFloatOrPercent(0, false));

    def = definition.add("first_layer_min_speed", coFloat, ptFFF);
    def->label = L("Min");
    def->full_label = L("Min first layer speed");
    def->category = OptionCategory::speed;
    def->invalidates_step = psGCodeExport;
    def->tooltip = L("Minimum speed when printing the first layer."
        "\nSet zero to disable.");
    def->sidetext = L("mm/s");
    def->min = 0;
    def->mode = comAdvancedE | comSuSi;
    def->set_default_value(new ConfigOptionFloat(0));
    
    def = definition.add("first_layer_temperature", coInts, ptFFF);
    def->label = L("First layer");
    def->full_label = L("First layer nozzle temperature");
    def->category = OptionCategory::filament;
    def->invalidates_step = psSkirtBrim;
    def->tooltip = L("Extruder nozzle temperature for first layer. If you want to control temperature manually "
                   "during print, set zero to disable temperature control commands in the output file.");
    def->sidetext = L("°C");
    def->min = 0;
    def->max = max_temp;
    def->is_vector_extruder = true;
    def->mode = comSimpleAE | comPrusa;
    def->set_default_value(new ConfigOptionInts { 200 });

    def = definition.add("full_fan_speed_layer", coInts, ptFFF);
    def->label = L("Full fan speed at layer");
    def->category = OptionCategory::filament;
    def->invalidates_step = psGCodeExport;
    def->tooltip = L("Fan speed will be ramped up linearly from zero at layer \"disable_fan_first_layers\" "
                   "to maximum at layer \"full_fan_speed_layer\". "
                   "\"full_fan_speed_layer\" will be ignored if equal or lower than \"disable_fan_first_layers\", in which case "
                   "the fan will be running at maximum allowed speed at layer \"disable_fan_first_layers\" + 1."
                   "\nset 0 to disable");
    def->min = 0;
    def->max = 1000;
    def->mode = comExpert | comPrusa;
    def->is_vector_extruder = true;
    def->set_default_value(new ConfigOptionInts { 4 });

    def = definition.add("fuzzy_skin", coEnum, ptFFF);
    def->label = L("Fuzzy Skin");
    def->category = OptionCategory::fuzzy_skin;
    def->invalidates_step = posSlice;
    def->tooltip = L("Fuzzy skin type.");
    def->set_enum<FuzzySkinType>({
        { "none",       L("None") },
        { "external",   L("Outside walls") },
        { "shell",      L("External walls") },
        { "all",        L("All walls") }
    });
    def->mode = comSimpleAE | comPrusa;
    def->set_default_value(new ConfigOptionEnum<FuzzySkinType>(FuzzySkinType::None));

    def = definition.add("fuzzy_skin_thickness", coFloatOrPercent, ptFFF);
    def->label = L("Fuzzy skin thickness");
    def->category = OptionCategory::fuzzy_skin;
    def->invalidates_step = posSlice;
    def->tooltip = L("The maximum distance that each skin point can be offset (both ways), "
        "measured perpendicular to the perimeter wall."
        "\nCan be a % of the nozzle diameter.");
    def->sidetext = L("mm or %");
    def->min = 0;
    def->mode = comAdvancedE | comPrusa;
    def->set_default_value(new ConfigOptionFloatOrPercent(150, true));

    def = definition.add("fuzzy_skin_point_dist", coFloatOrPercent, ptFFF);
    def->label = L("Fuzzy skin point distance");
    def->category = OptionCategory::fuzzy_skin;
    def->invalidates_step = posSlice;
    def->tooltip = L("Perimeters will be split into multiple segments by inserting Fuzzy skin points. "
        "Lowering the Fuzzy skin point distance will increase the number of randomly offset points on the perimeter wall."
        "\nCan be a % of the nozzle diameter.");
    def->sidetext = L("mm or %");
    def->min = 0;
    def->mode = comAdvancedE | comPrusa;
    def->set_default_value(new ConfigOptionFloatOrPercent(200, true));

    def = definition.add("gap_fill_enabled", coBool, ptFFF);
    def->label = L("Gap fill");
    def->full_label = L("Enable Gap fill");
    def->category = OptionCategory::perimeter;
    def->invalidates_step = posSlice;
    def->tooltip = L("Enable gap fill algorithm. It will extrude small lines between perimeters "
        "when there is not enough space for another perimeter or an infill.");
    def->mode = comAdvancedE | comPrusa;
    def->aliases = { "gap_fill" }; //superslicer 2.3 or older
    def->set_default_value(new ConfigOptionBool(true));

    def = definition.add("gap_fill_acceleration", coFloatOrPercent, ptFFF);
    def->label = L("Gap fill");
    def->full_label = L("Gap fill acceleration");
    def->category = OptionCategory::speed;
    def->invalidates_step = psGCodeExport;
    def->tooltip = L("This is the acceleration your printer will use for gap fills. "
                "\nThis can be expressed as a percentage over the perimeter acceleration."
                "\nSet zero to use perimeter acceleration for gap fills.");
    def->sidetext = L("mm/s² or %");
    def->ratio_over = "perimeter_acceleration";
    def->min = 0;
    def->max_literal = { -200, false };
    def->mode = comExpert | comSuSi;
    def->set_default_value(new ConfigOptionFloatOrPercent(0,false));

    def = definition.add("gap_fill_extension", coFloatOrPercent, ptFFF);
    def->label = L("Extension");
    def->full_label = L("Gap fill: extra extension");
    def->category = OptionCategory::perimeter;
    def->invalidates_step = posPerimeters;
    def->tooltip = L("Increase the length of all gapfills by this amount (may overextrude a little bit)"
        "\nCan be a % of the extrusion width"
        "\nIs also used by infill's gapfill.");
    def->ratio_over = "perimeter_width";
    def->sidetext = L("mm or %");
    def->min = 0;
    def->max_literal = { 50, true };
    def->mode = comExpert | comSuSi;
    def->set_default_value(new ConfigOptionFloatOrPercent{ 0, false });

    def = definition.add("gap_fill_fan_speed", coInts, ptFFF);
    def->label = L("Gap fill fan speed");
    def->category = OptionCategory::cooling;
    def->invalidates_step = psGCodeExport;
    def->tooltip = L("This fan speed is enforced during all gap fill Perimeter moves"
        "\nSet to 0 to stop the fan."
        "\nIf disabled, default fan speed will be used."
        "\nCan be disabled by disable_fan_first_layers, slowed down by full_fan_speed_layer and increased by low layer time.");
    def->sidetext = L("%");
    def->min = 0;
    def->max = 100;
    def->mode = comExpert | comSuSi;
    def->is_vector_extruder = true;
    def->can_be_disabled = true;
    def->set_default_value(disable_default_option(new ConfigOptionInts({ 100 })));

    def = definition.add("gap_fill_flow_match_perimeter", coPercent, ptFFF);
    def->label = L("Cap with perimeter flow");
    def->full_label = L("Gapfill: cap speed with perimeter flow");
    def->category = OptionCategory::speed;
    def->invalidates_step = psGCodeExport;
    def->tooltip = L("A percentage of the perimeter flow (mm3/s) is used as a limit for the gap fill flow, and so the gapfill may reduce its speed when the gap fill extrusions became too thick."
                " This allow you to use a high gapfill speed, to print the thin gapfill quickly and reduce the difference in flow rate for the gapfill."
                "\nSet zero to deactivate.");
    def->sidetext = L("%");
    def->min = 0;
    def->mode = comExpert | comSuSi;
    def->set_default_value(new ConfigOptionPercent(0));

    def = definition.add("gap_fill_last", coBool, ptFFF);
    def->label = L("after last perimeter");
    def->full_label = L("Gapfill: after last perimeter");
    def->category = OptionCategory::perimeter;
    def->invalidates_step = posPerimeters;
    def->tooltip = L("All gaps, between the last perimeter and the infill, which are thinner than a perimeter will be filled by gapfill.");
    def->mode = comExpert | comSuSi;
    def->set_default_value(new ConfigOptionBool(false));

    def = definition.add("gap_fill_max_width", coFloatOrPercent, ptFFF);
    def->label = L("Max width");
    def->full_label = L("Gapfill: Max width");
    def->category = OptionCategory::perimeter;
    def->invalidates_step = posPerimeters;
    def->tooltip = L("This setting represents the maximum width of a gapfill. Points wider than this threshold won't be created."
        "\nCan be a % of the extrusion width"
        "\n0 to auto"
        "\nIs also used by infill's gapfill.");
    def->ratio_over = "perimeter_width";
    def->sidetext = L("mm or %");
    def->min = 0;
    def->mode = comExpert | comSuSi;
    def->set_default_value(new ConfigOptionFloatOrPercent{ 0, false });

    def = definition.add("gap_fill_min_area", coFloatOrPercent, ptFFF);
    def->label = L("Min surface");
    def->full_label = L("Gapfill: Min surface");
    def->category = OptionCategory::perimeter;
    def->invalidates_step = posPerimeters;
    def->tooltip = L("This setting represents the minimum mm² for a gapfill extrusion to be created."
        "\nCan be a % of (extrusion width)²"
        "\nIs also used by infill's gapfill.");
    def->ratio_over = "perimeter_width_square";
    def->sidetext = L("mm² or %");
    def->min = 0;
    def->mode = comExpert | comSuSi;
    def->set_default_value(new ConfigOptionFloatOrPercent{ 100, true });

    def = definition.add("gap_fill_min_length", coFloatOrPercent, ptFFF);
    def->label = L("Min length");
    def->full_label = L("Gapfill: Min length");
    def->category = OptionCategory::perimeter;
    def->invalidates_step = posPerimeters;
    def->tooltip = L("This setting represents the minimum mm for a gapfill extrusion to be extruded."
        "\nCan be a % of the extrusion width"
        "\n0 to auto"
        "\nIs also used by infill's gapfill.");
    def->ratio_over = "perimeter_width";
    def->sidetext = L("mm or %");
    def->min = 0;
    def->mode = comExpert | comSuSi;
    def->set_default_value(new ConfigOptionFloatOrPercent{ 0, false });

    def = definition.add("gap_fill_min_width", coFloatOrPercent, ptFFF);
    def->label = L("Min width");
    def->full_label = L("Gapfill: Min width");
    def->category = OptionCategory::perimeter;
    def->invalidates_step = posPerimeters;
    def->tooltip = L("This setting represents the minimum width of a gapfill. Points thinner than this threshold won't be created."
        "\nCan be a % of the extrusion width"
        "\n0 to auto"
        "\nIs also used by infill's gapfill.");
    def->ratio_over = "perimeter_width";
    def->sidetext = L("mm or %");
    def->min = 0;
    def->mode = comExpert | comSuSi;
    def->set_default_value(new ConfigOptionFloatOrPercent{ 0, false });

    def = definition.add("gap_fill_no_overhang", coBool, ptFFF);
    def->label = L("No gap fill on overhang areas");
    def->category = OptionCategory::perimeter;
    def->invalidates_step = posPerimeters;
    def->tooltip = L("Prevent gapfill into overhang areas. May create some holes.");
    def->mode = comExpert | comSuSi;
    def->set_default_value(new ConfigOptionBool(false));

    def = definition.add("gap_fill_overlap", coPercent, ptFFF);
    def->label = L("Gap fill overlap");
    def->full_label = L("Gap fill overlap");
    def->category = OptionCategory::width;
    def->invalidates_step = posPerimeters;
    def->tooltip = L("This setting allows you to reduce the overlap between the perimeters and the gap fill."
        " 100% means that no gaps are left, and 0% means that the gap fill won't touch the perimeters."
        "\nMay be useful if you can see the gapfill on the exterrnal surface, to reduce that artifact.");
    def->sidetext = L("%");
    def->min = 0;
    def->max = 100;
    def->mode = comExpert | comSuSi;
    def->set_default_value(new ConfigOptionPercent(80));

    def = definition.add("gap_fill_perimeter", coBool, ptFFF);
    def->label = L("Allow Perimeter inside Gap fill");
    def->full_label = L("Allow Perimeter inside Gap fill");
    def->category = OptionCategory::perimeter;
    def->invalidates_step = posPerimeters;
    def->tooltip = L("Allow to create a perimeter inside a gapfill area if it's possible.");
    def->mode = comExpert | comSuSi;
    def->set_default_value(new ConfigOptionBool(true));

    def = definition.add("gap_fill_speed", coFloatOrPercent, ptFFF);
    def->label = L("Gap fill");
    def->full_label = L("Gap fill speed");
    def->category = OptionCategory::speed;
    def->invalidates_step = posSlice;
    def->tooltip = L("Speed for filling small gaps using short zigzag moves. Keep this reasonably low "
        "to avoid too much shaking and resonance issues."
        "\nGap fill extrusions are ignored from the automatic volumetric speed computation, unless you set it to 0."
        "\nThis can be expressed as a percentage (for example: 80%) over the Internal Perimeter speed.");
    def->sidetext = L("mm/s or %");
    def->ratio_over = "perimeter_speed";
    def->min = 0;
    def->mode = comExpert | comPrusa;
    def->set_default_value(new ConfigOptionFloatOrPercent(50,true));

    def = definition.add("gcode_ascii", coBool, ptFFF);
    def->label = L("Only ascii characters in gcode");
    def->category = OptionCategory::firmware;
    def->invalidates_step = psGCodeExport;
    def->tooltip = L("When printing the gcode file, replace any non-ascii character by a '_'."
        " Can be useful if the firmware or a software in a workflow doesn't support UTF-8.");
    def->mode = comExpert | comSuSi;
    def->set_default_value(new ConfigOptionBool(false));

    def = definition.add("gcode_comments", coBool, ptFFF);
    def->label = L("Verbose G-code");
    def->category = OptionCategory::output;
    def->invalidates_step = psGCodeExport;
    def->tooltip = L("Enable this to get a commented G-code file, with each line explained by descriptive text. "
        "If you print from an SD card, the additional weight of the file could make your firmware "
        "slow down.");
    def->mode = comExpert | comPrusa;
    def->set_default_value(new ConfigOptionBool(0));

    def = definition.add("gcode_filename_illegal_char", coString, ptFFF);
    def->label = L("Illegal characters");
    def->full_label = L("Illegal characters for filename");
    def->category = OptionCategory::output;
    def->invalidates_step = psGCodeExport;
    def->tooltip = L("All characters that are written here will be replaced by '_' when writing the gcode file name."
        "\nIf the first charater is '[' or '(', then this field will be considered as a regexp (enter '[^a-zA-Z0-9]' to only use ascii char).");
    def->mode = comExpert | comSuSi;
    def->set_default_value(new ConfigOptionString("[<>:\"/\\\\|?*]"));

    def = definition.add("gcode_flavor", coEnum, ptFFF);
    def->label = L("G-code flavor");
    def->category = OptionCategory::general;
    def->invalidates_step = psSkirtBrim;
    def->tooltip = L("Some G/M-code commands, including temperature control and others, are not universal. "
                   "Set this option to your printer's firmware to get a compatible output. "
                   "The \"No extrusion\" flavor prevents Slic3r from exporting any extrusion value at all.");
    def->set_enum<GCodeFlavor>({
        { "sprinter",       "RepRap/Sprinter" },
        { "reprapfirmware", "RepRapFirmware" },
        { "repetier",       "Repetier" },
        { "teacup",         "Teacup" },
        { "makerware",      "MakerWare (MakerBot)" },
        { "marlin",         "Marlin (legacy)" },
        { "marlin2",        "Marlin 2" },
        { "klipper",        "Klipper" },
        { "sailfish",       "Sailfish (MakerBot)" },
        { "mach3",          "Mach3/LinuxCNC" },
        { "machinekit",     "Machinekit" },
        { "smoothie",       "Smoothie" },
        { "no-extrusion",   L("No extrusion") }
    });
    def->mode = comAdvancedE | comPrusa;
    def->set_default_value(new ConfigOptionEnum<GCodeFlavor>(gcfMarlinLegacy));

    def = definition.add("gcode_label_objects", coEnum, ptFFF);
    def->label = L("Label objects");
    def->invalidates_step = psGCodeExport;
    def->tooltip = L("Selects whether labels should be exported at object boundaries and in what format.\n"
                     "OctoPrint = comments to be consumed by OctoPrint CancelObject plugin.\n"
                     "Firmware = firmware specific G-code (it will be chosen based on firmware flavor and it can end up to be empty).\n\n"
                     "This settings is NOT compatible with Single Extruder Multi Material setup and Wipe into Object / Wipe into Infill.");

    def->set_enum<LabelObjectsStyle>({
        { "disabled",   L("Disabled") },
        { "octoprint",  L("OctoPrint comments") },
        { "firmware",   L("Firmware-specific") },
        { "both",       L("Print both") }
        });
    def->mode = comAdvancedE | comPrusa;
    def->set_default_value(new ConfigOptionEnum<LabelObjectsStyle>(LabelObjectsStyle::Octoprint));

    def = definition.add("gcode_precision_xyz", coInt, ptFFF);
    def->label = L("xyz decimals");
    def->category = OptionCategory::output;
    def->invalidates_step = psGCodeExport;
    def->tooltip = L("Choose how many digits after the dot for xyz coordinates.");
    def->min = 0;
    def->max = 7;
    def->mode = comExpert | comSuSi;
    def->set_default_value(new ConfigOptionInt(3));

    def = definition.add("gcode_precision_e", coInt, ptFFF);
    def->label = L("Extruder decimals");
    def->category = OptionCategory::output;
    def->invalidates_step = psGCodeExport;
    def->tooltip = L("Choose how many digits after the dot for extruder moves.");
    def->min = 0;
    def->max = 7;
    def->mode = comExpert | comSuSi;
    def->set_default_value(new ConfigOptionInt(5));

    def = definition.add("gcode_substitutions", coStrings, ptFFF);
    def->label = L("G-code substitutions");
    def->invalidates_step = psGCodeExport;
    def->tooltip = L("Find / replace patterns in G-code lines and substitute them.");
    def->mode = comExpert | comPrusa;
    def->set_default_value(new ConfigOptionStrings());

    def = definition.add("high_current_on_filament_swap", coBool, ptFFF);
    def->label = L("High extruder current on filament swap");
    def->category = OptionCategory::general;
    def->invalidates_step = psSkirtBrim;
    def->tooltip = L("It may be beneficial to increase the extruder motor current during the filament exchange"
                   " sequence to allow for rapid ramming feed rates and to overcome resistance when loading"
                   " a filament with an ugly shaped tip.");
    def->mode = comExpert | comPrusa;
    def->set_default_value(new ConfigOptionBool(0));

    def = definition.add("infill_acceleration", coFloatOrPercent, ptFFF);
    def->label = L("Sparse");
    def->full_label = L("Infill acceleration");
    def->category = OptionCategory::speed;
    def->invalidates_step = psGCodeExport;
    def->tooltip = L("This is the acceleration your printer will use for Sparse infill."
                "\nCan be a % of the solid infill acceleration"
                "\nSet zero to use solid infill acceleration for infill.");
    def->sidetext = L("mm/s² or %");
    def->ratio_over = "solid_infill_acceleration";
    def->min = 0;
    def->max_literal = { -200, false };
    def->mode = comAdvancedE | comPrusa;
    def->set_default_value(new ConfigOptionFloatOrPercent(0,false));

    def = definition.add("infill_every_layers", coInt, ptFFF);
    def->label = L("Combine infill every");
    def->category = OptionCategory::infill;
    def->invalidates_step = posPrepareInfill;
    def->tooltip = L("This feature allows you to combine infill and speed up your print by extruding thicker "
                   "infill layers while preserving thin perimeters, thus accuracy.");
    def->sidetext = L("layers");
    def->full_label = L("Combine infill every n layers");
    def->min = 1;
    def->mode = comAdvancedE | comPrusa;
    def->set_default_value(new ConfigOptionInt(1));

    def = definition.add("idle_temperature", coInts, ptFFF);
    def->label = L("Idle temperature");
    def->tooltip = L("Nozzle temperature when the tool is currently not used in multi-tool setups."
                     "\nThis is only used when 'Ooze prevention' is active in Print Settings.");
    def->sidetext = L("°C");
    def->category = OptionCategory::filament;
    def->invalidates_step = psSkirtBrim;
    def->min = 0;
    def->max = max_temp;
    def->can_be_disabled = true;
    def->mode = comSimpleAE | comPrusa;
    def->is_vector_extruder = true;
    def->set_default_value(disable_default_option(new ConfigOptionInts{30}));

    auto def_infill_anchor_min = def = definition.add("infill_anchor", coFloatOrPercent, ptFFF);
    def->label = L("Length of the infill anchor");
    def->category = OptionCategory::infill;
    def->invalidates_step = posInfill;
    def->tooltip = L("Connect an infill line to an internal perimeter with a short segment of an additional perimeter. "
                     "If expressed as percentage (example: 15%) it is calculated over infill extrusion width. Slic3r tries to connect two close infill lines to a short perimeter segment. If no such perimeter segment "
                     "shorter than infill_anchor_max is found, the infill line is connected to a perimeter segment at just one side "
                     "and the length of the perimeter segment taken is limited to this parameter, but no longer than anchor_length_max. "
                     "\nSet this parameter to zero to disable anchoring perimeters connected to a single infill line.");
    def->sidetext = L("mm or %");
    def->ratio_over = "infill_extrusion_width";
    def->max_literal = { 1000, false };
    def->set_enum_values(ConfigOptionDef::GUIType::f_enum_open, {
        { "0",      L("0 (no open anchors)") },
        { "1",      L("1 mm") },
        { "2",      L("2 mm") },
        { "5",      L("5 mm") },
        { "10",     L("10 mm") },
        { "1000",   L("1000 (unlimited)") }
    });
    def->mode = comExpert | comPrusa;
    def->set_default_value(new ConfigOptionFloatOrPercent(600, true));

    def = definition.add("infill_anchor_max", coFloatOrPercent, ptFFF);
    def->label = L("Maximum length of the infill anchor");
    def->category    = def_infill_anchor_min->category;
    def->invalidates_step = posInfill;
    def->tooltip = L("Connect an infill line to an internal perimeter with a short segment of an additional perimeter. "
                     "If expressed as percentage (example: 15%) it is calculated over infill extrusion width. Slic3r tries to connect two close infill lines to a short perimeter segment. If no such perimeter segment "
                     "shorter than this parameter is found, the infill line is connected to a perimeter segment at just one side "
                     "and the length of the perimeter segment taken is limited to infill_anchor, but no longer than this parameter. "
                     "\nIf set to 0, the old algorithm for infill connection will be used, it should create the same result as with 1000 & 0.");
    def->sidetext    = def_infill_anchor_min->sidetext;
    def->ratio_over  = def_infill_anchor_min->ratio_over;
    def->set_enum_values(ConfigOptionDef::GUIType::f_enum_open, {
        { "0",      L("0 (Simple connect)") },
        { "1",      L("1 mm") },
        { "2",      L("2 mm") },
        { "5",      L("5 mm") },
        { "10",     L("10 mm") },
        { "1000",   L("1000 (unlimited)") }
    });
    def->mode = comAdvancedE | comPrusa;
    def->set_default_value(new ConfigOptionFloatOrPercent(0, false));

    def = definition.add("infill_connection", coEnum, ptFFF);
    def->label = L("Connection of sparse infill lines");
    def->category = OptionCategory::infill;
    def->invalidates_step = posInfill;
    def->tooltip = L("Give to the infill algorithm if the infill needs to be connected, and on which perimeters"
        " Can be useful for art or with high infill/perimeter overlap."
        " The result may vary between infill types.");
    def->set_enum<InfillConnection>({
        { "connected", L("Connected") },
        { "holes", L("Connected to hole perimeters") },
        { "outershell", L("Connected to outer perimeters") },
        { "notconnected", L("Not connected") },
    });
    def->mode = comExpert | comSuSi;
    def->set_default_value(new ConfigOptionEnum<InfillConnection>(icConnected));

    def = definition.add("infill_connection_top", coEnum, ptFFF);
    def->label = L("Connection of top infill lines");
    def->category = OptionCategory::infill;
    def->invalidates_step = posInfill;
    def->tooltip = L("Give to the infill algorithm if the infill needs to be connected, and on which perimeters"
        " Can be useful for art or with high infill/perimeter overlap."
        " The result may vary between infill types.");
    def->set_enum<InfillConnection>({
        { "connected", L("Connected") },
        { "holes", L("Connected to hole perimeters") },
        { "outershell", L("Connected to outer perimeters") },
        { "notconnected", L("Not connected") },
    });
    def->mode = comExpert | comSuSi;
    def->set_default_value(new ConfigOptionEnum<InfillConnection>(icConnected));

    def = definition.add("infill_connection_bottom", coEnum, ptFFF);
    def->label = L("Connection of bottom infill lines");
    def->category = OptionCategory::infill;
    def->invalidates_step = posInfill;
    def->tooltip = L("Give to the infill algorithm if the infill needs to be connected, and on which perimeters"
        " Can be useful for art or with high infill/perimeter overlap."
        " The result may vary between infill types.");
    def->set_enum<InfillConnection>({
        { "connected", L("Connected") },
        { "holes", L("Connected to hole perimeters") },
        { "outershell", L("Connected to outer perimeters") },
        { "notconnected", L("Not connected") },
    });
    def->mode = comExpert | comSuSi;
    def->set_default_value(new ConfigOptionEnum<InfillConnection>(icConnected));

    def = definition.add("infill_connection_bridge", coEnum, ptFFF);
    def->label = L("Connection of bridged infill lines");
    def->category = OptionCategory::infill;
    def->invalidates_step = posInfill;
    def->tooltip = L("Give to the bridge infill algorithm if the infill needs to be connected, and on which perimeters."
        " Can be useful to disconnect to reduce a little bit the pressure buildup when going over the bridge's anchors.");
    def->set_enum<InfillConnection>({
        { "connected", L("Connected") },
        { "holes", L("Connected to hole perimeters") },
        { "outershell", L("Connected to outer perimeters") },
        { "notconnected", L("Not connected") },
    });
    def->mode = comExpert | comSuSi;
    def->set_default_value(new ConfigOptionEnum<InfillConnection>(icNotConnected));

    def = definition.add("infill_connection_solid", coEnum, ptFFF);
    def->label = L("Connection of solid infill lines");
    def->category = OptionCategory::infill;
    def->invalidates_step = posInfill;
    def->tooltip = L("Give to the infill algorithm if the infill needs to be connected, and on which perimeters"
        " Can be useful for art or with high infill/perimeter overlap."
        " The result may vary between infill types.");
    def->set_enum<InfillConnection>({
        { "connected", L("Connected") },
        { "holes", L("Connected to hole perimeters") },
        { "outershell", L("Connected to outer perimeters") },
        { "notconnected", L("Not connected") },
    });
    def->mode = comExpert | comSuSi;
    def->set_default_value(new ConfigOptionEnum<InfillConnection>(icConnected));

    def = definition.add("infill_dense", coBool, ptFFF);
    def->label = L("Dense infill layer");
    def->full_label = L("Dense infill layer");
    def->category = OptionCategory::infill;
    def->invalidates_step = posPrepareInfill;
    def->tooltip = L("Enables the creation of a support layer under the first solid layer. This allows you to use a lower infill ratio without compromising the top quality."
        " The dense infill is laid out with a 50% infill density.");
    def->mode = comSimpleAE | comSuSi;
    def->set_default_value(new ConfigOptionBool(false));
    
    // Deprecated: the dense-infill algorithm is moving to a plugin-owned
    // option. Keep this legacy definition while the old pipeline still reads
    // it; plugin option registration is idempotent when the existing
    // definition is compatible.
    def = definition.add("infill_dense_algo", coEnum, ptFFF);
    def->label = L("Algorithm");
    def->full_label = L("Dense infill algorithm");
    def->category = OptionCategory::infill;
    def->invalidates_step = posPrepareInfill;
    def->tooltip = L("Choose the way the dense layer is laid out."
        " The automatic option lets it try to draw the smallest surface with only straight lines inside the sparse infill."
        " The Anchored option just slightly enlarges (by 'Default infill margin') the surfaces that need a better support.");
    def->set_enum<DenseInfillAlgo>({
        { "automatic", L("Automatic") },
        { "autonotfull", L("Automatic, unless full") },
        { "autosmall", L("Automatic, only for small areas") },
        { "autoenlarged", L("Automatic, or anchored if too big") },
        { "enlarged", L("Anchored") },
    });
    def->mode = comAdvancedE | comSuSi;
    def->set_default_value(new ConfigOptionEnum<DenseInfillAlgo>(dfaAutoOrEnlarged));

    def = definition.add("infill_extruder", coInt, ptFFF);
    def->label = L("Infill extruder");
    def->category = OptionCategory::extruders;
    def->invalidates_step = posPrepareInfill;
    def->tooltip = L("The extruder to use when printing infill.");
    def->min = 1;
    def->mode = comAdvancedE | comPrusa;
    def->set_default_value(new ConfigOptionInt(1));

    def = definition.add("infill_extrusion_width", coFloatOrPercent, ptFFF);
    def->label = L("Infill");
    def->full_label = L("Infill width");
    def->category = OptionCategory::width;
    def->invalidates_step = posPrepareInfill;
    def->tooltip = L("Set this to a non-zero value to set a manual extrusion width for infill. "
        "If left as zero, default extrusion width will be used if set, otherwise 1.125 x nozzle diameter will be used. "
        "You may want to use fatter extrudates to speed up the infill and make your parts stronger. "
        "If expressed as percentage (for example 110%) it will be computed over nozzle diameter."
        "\nYou can set either 'Spacing', or 'Width'; the other will be calculated, using default layer height.");
    def->sidetext = L("mm or %");
    def->ratio_over = "nozzle_diameter";
    def->min = 0;
    def->max = 1000;
    def->max_literal = { 10, true };
    def->precision = 6;
    def->can_phony = true;
    def->mode = comAdvancedE | comPrusa;
    def->set_default_value((new ConfigOptionFloatOrPercent(0, false))->set_phony(true));

    def = definition.add("infill_extrusion_change_odd_layers", coFloatOrPercent, ptFFF);
    def->label = L("Infill");
    def->full_label = L("Infill spacing change on even layers");
    def->category = OptionCategory::width;
    def->invalidates_step = posPrepareInfill;
    def->tooltip = L("Change width on every even layer (and not on odd layers like the first one) for better overlap with adjacent layers and getting stringer shells. "
                     "Try values about +/- 0.1 with different sign."
                     "\nThis could be combined with extra permeters on even layers."
                     "\nWorks as absolute spacing or a % of the spacing."
                     "\nset 0 to disable");
    def->sidetext = L("mm or %");
    def->mode = comExpert | comSuSi;
    def->set_default_value(new ConfigOptionFloatOrPercent(false, 0));

    def = definition.add("infill_extrusion_spacing", coFloatOrPercent, ptFFF);
    def->label = L("Infill");
    def->full_label = L("Infill spacing");
    def->category = OptionCategory::width;
    def->invalidates_step = posPrepareInfill;
    def->tooltip = L("Like First layer width but spacing is the distance between two lines (as they overlap a bit, it's not the same)."
         "\nYou can set either 'Spacing', or 'Width'; the other will be calculated, using default layer height.");
    def->sidetext = L("mm or %");
    def->ratio_over = "nozzle_diameter";
    def->min = 0;
    def->max = 1000;
    def->max_literal = { 10, true };
    def->precision = 6;
    def->can_phony = true;
    def->mode = comAdvancedE | comSuSi;
    def->set_default_value(new ConfigOptionFloatOrPercent(100, true));

    def = definition.add("infill_fan_speed", coInts, ptFFF);
    def->label = L("Internal Infill fan speed");
    def->category = OptionCategory::cooling;
    def->invalidates_step = psGCodeExport;
    def->tooltip = L("This fan speed is enforced during all Internal Infill moves"
        "\nSet to 0 to stop the fan."
        "\nIf disabled, default fan speed will be used."
        "\nCan be disabled by disable_fan_first_layers, slowed down by full_fan_speed_layer and increased by low layer time.");
    def->sidetext = L("%");
    def->min = 0;
    def->max = 100;
    def->mode = comExpert | comSuSi;
    def->is_vector_extruder = true;
    def->can_be_disabled = true;
    def->set_default_value(disable_default_option(new ConfigOptionInts({ 100 })));

    def = definition.add("infill_filled_bottom", coBool, ptFFF);
    def->label = L("GapFill for bottom infill areas");
    def->category = OptionCategory::infill;
    def->invalidates_step = posInfill;
    def->tooltip = L("On bottom infill areas, add gapfill where the infill pattern can't go.");
    def->mode = comExpert | comSuSi;
    def->set_default_value(new ConfigOptionBool(false));

    def = definition.add("infill_filled_solid", coBool, ptFFF);
    def->label = L("GapFill for solid infill areas");
    def->category = OptionCategory::infill;
    def->invalidates_step = posInfill;
    def->tooltip = L("On solid infill areas, add gapfill where the infill pattern can't go.");
    def->mode = comExpert | comSuSi;
    def->set_default_value(new ConfigOptionBool(true));

    def = definition.add("infill_filled_top", coBool, ptFFF);
    def->label = L("GapFill for top infill areas");
    def->category = OptionCategory::infill;
    def->invalidates_step = posInfill;
    def->tooltip = L("On top infill areas, add gapfill where the infill pattern can't go.");
    def->mode = comExpert | comSuSi;
    def->set_default_value(new ConfigOptionBool(false));

    def = definition.add("infill_first", coBool, ptFFF);
    def->label = L("Infill before perimeters");
    def->category = OptionCategory::infill;
    def->invalidates_step = psWipeTower;
    def->tooltip = L("This option will switch the print order of perimeters and infill, making the latter first.");
    def->mode = comExpert | comPrusa;
    def->set_default_value(new ConfigOptionBool(false));

    //def = definition.add("infill_only_where_needed", coBool);
    def->invalidates_step = posPrepareInfill;
    //def->label = L("Only infill where needed");
    //def->category = OptionCategory::infill;
    //def->tooltip = L("This option will limit infill to the areas actually needed for supporting ceilings "
    //               "(it will act as internal support material). If enabled, this slows down the G-code generation "
    //               "due to the multiple checks involved.");
    //def->mode = comAdvancedE | comPrusa;
    //def->set_default_value(new ConfigOptionBool(false));

    def = definition.add("infill_overlap", coFloatOrPercent, ptFFF);
    def->label = L("Infill/perimeters encroachment");
    def->category = OptionCategory::width;
    def->invalidates_step = posPerimeters;
    def->tooltip = L("This setting applies an additional overlap between infill and perimeters for better bonding. "
                   "Theoretically this shouldn't be needed, but backlash might cause gaps. If expressed "
                   "as percentage (example: 15%) it is calculated over perimeter extrusion width."
                    "\nDon't put a value higher than 50% (of the perimeter width), as it will fuse with it and follow the perimeter.");
    def->sidetext = L("mm or %");
    def->ratio_over = "perimeter_extrusion_width";
    def->min = 0;
    def->max_literal = { 0.5, true };
    def->mode = comExpert | comPrusa;
    def->set_default_value(new ConfigOptionFloatOrPercent(25, true));

    def = definition.add("infill_speed", coFloatOrPercent, ptFFF);
    def->label = L("Sparse");
    def->full_label = L("Sparse infill speed");
    def->category = OptionCategory::speed;
    def->invalidates_step = psGCodeExport;
    def->tooltip = L("Speed for printing the internal fill."
        "\nThis can be expressed as a percentage (for example: 80%) over the Solid Infill speed."
        "\nSet zero to use autospeed for this feature.");
    def->sidetext = L("mm/s or %");
    def->ratio_over = "solid_infill_speed";
    def->aliases = { "print_feed_rate", "infill_feed_rate" };
    def->min = 0;
    def->mode = comAdvancedE | comPrusa;
    def->set_default_value(new ConfigOptionFloatOrPercent(300, true));

    def = definition.add("inherits", coString, ptFFF);
    def->label = L("Inherits profile");
    def->tooltip = L("Name of the profile, from which this profile inherits.");
    def->full_width = true;
    def->height = 5;
    def->set_default_value(new ConfigOptionString());
    def->cli = ConfigOptionDef::nocli;

    // The following value is to be stored into the project file (AMF, 3MF, Config ...)
    // and it contains a sum of "inherits" values over the print and filament profiles.
    def = definition.add("inherits_cummulative", coStrings, ptFFF);
    def->set_default_value(new ConfigOptionStrings());
    def->mode = comPrusa;
    def->cli = ConfigOptionDef::nocli;

    def = definition.add("interface_shells", coBool, ptFFF);
    def->label = L("Interface shells");
    def->tooltip = L("Force the generation of solid shells between adjacent materials/volumes. "
                   "Useful for multi-extruder prints with translucent materials or manual soluble "
                   "support material.");
    def->category = OptionCategory::perimeter;
    def->invalidates_step = posPrepareInfill;
    def->mode = comExpert | comPrusa;
    def->set_default_value(new ConfigOptionBool(false));

    def = definition.add("internal_bridge_acceleration", coFloatOrPercent, ptFFF);
    def->label = L("Internal bridges ");
    def->full_label = L("Internal bridges acceleration");
    def->category = OptionCategory::speed;
    def->invalidates_step = psGCodeExport;
    def->tooltip = L("This is the acceleration your printer will use for internal bridges. "
                "\nCan be a % of the default acceleration"
                "\nSet zero to use bridge acceleration for internal bridges.");
    def->sidetext = L("mm/s² or %");
    def->ratio_over = "bridge_acceleration";
    def->min = 0;
    def->max_literal = { -200, false };
    def->mode = comExpert | comSuSi;
    def->set_default_value(new ConfigOptionFloatOrPercent(0,false));
    def->aliases = { "bridge_internal_acceleration" };

    def = definition.add("internal_bridge_expansion", coBool, ptFFF);
    def->label = L("Extends internal bridge to infill");
    def->category = OptionCategory::skirtBrim;
    def->tooltip = L("When creating internal bridges, extends the line to the nearest internal line it can use to support itself."
        " This way, it can avoid curling up as when bridges lines ends over a void.");
    def->category = OptionCategory::infill;
    def->invalidates_step = posPrepareInfill;
    def->mode = comAdvancedE | comSuSi;
    def->set_default_value(new ConfigOptionBool(true));

    def = definition.add("internal_bridge_fan_speed", coInts, ptFFF);
    def->label = L("Internal Bridge Infill fan speed");
    def->category = OptionCategory::cooling;
    def->invalidates_step = psGCodeExport;
    def->tooltip = L("This fan speed is enforced during all infill bridges. It won't slow down the fan if it's currently running at a higher speed."
        "\nSet to 0 to stop the fan."
        "\nIf disabled, Bridge fan speed will be used."
        "\nCan be disabled by disable_fan_first_layers and increased by low layer time.");
    def->sidetext = L("%");
    def->min = 0;
    def->max = 100;
    def->mode = comAdvancedE | comSuSi;
    def->is_vector_extruder = true;
    def->can_be_disabled = true;
    def->set_default_value(disable_default_option(new ConfigOptionInts({ 100 })));
    def->aliases = { "bridge_internal_fan_speed" };

    def = definition.add("internal_bridge_min_width", coFloatOrPercent, ptFFF);
    def->label = L("Internal bridge infill threshold width");
    def->category = OptionCategory::infill;
    def->invalidates_step = posPrepareInfill;
    def->tooltip = L("Minimum width for the solid infill to convert into an internal bridge infill."
                    "\nCan be a % of the current solid infill spacing.");
    def->sidetext = L("mm or %");
    def->min = 0;
    def->mode = comExpert | comSuSi;
    def->set_default_value(new ConfigOptionFloatOrPercent(300, true));

    def = definition.add("internal_bridge_speed", coFloatOrPercent, ptFFF);
    def->label = L("Internal Bridge Infill speed");
    def->category = OptionCategory::speed;
    def->invalidates_step = psGCodeExport;
    def->tooltip = L("Speed for printing the bridges that support the top layer.\nCan be a % of the bridge speed.");
    def->sidetext = L("mm/s or %");
    def->ratio_over = "bridge_speed";
    def->min = 0;
    def->mode = comExpert | comSuSi;
    def->set_default_value(new ConfigOptionFloatOrPercent(150,true));
    def->aliases = { "bridge_speed_internal" };

    def = definition.add("mmu_segmented_region_max_width", coFloat, ptFFF);
    def->label = L("Maximum width of a segmented region");
    def->tooltip = L("Maximum width of a segmented region. Zero disables this feature.");
    def->sidetext = L("mm (zero to disable)");
    def->min = 0;
    def->category = OptionCategory::mmsetup;
    def->invalidates_step = posSlice;
    def->mode = comExpert | comPrusa;
    def->set_default_value(new ConfigOptionFloat(0.));

    def = definition.add("mmu_segmented_region_interlocking_depth", coFloat, ptFFF);
    def->label = L("Interlocking depth of a segmented region");
    def->tooltip = L("Interlocking depth of a segmented region. It will be ignored if "
                       "\"mmu_segmented_region_max_width\" is zero or if \"mmu_segmented_region_interlocking_depth\""
                       "is bigger than \"mmu_segmented_region_max_width\". Zero disables this feature.");
    def->sidetext = L("mm (zero to disable)");
    def->min = 0;
    def->category = OptionCategory::mmsetup;
    def->mode = comExpert | comPrusa;
    def->set_default_value(new ConfigOptionFloat(0.));

    def = definition.add("ironing", coBool, ptFFF);
    def->label = L("Enable ironing");
    def->tooltip = L("Enable ironing of the top layers with the hot print head for smooth surface");
    def->category = OptionCategory::ironing;
    def->invalidates_step = posPrepareInfill;
    def->mode = comAdvancedE | comPrusa;
    def->set_default_value(new ConfigOptionBool(false));

    def = definition.add("ironing_acceleration", coFloatOrPercent, ptFFF);
    def->label = L("Ironing");
    def->full_label = L("Ironing acceleration");
    def->category = OptionCategory::ironing;
    def->invalidates_step = psGCodeExport;
    def->tooltip = L("This is the acceleration your printer will use for ironing. "
                "\nCan be a % of the top solid infill acceleration"
                "\nSet zero or 100% to use top solid infill acceleration for ironing.");
    def->sidetext = L("mm/s² or %");
    def->ratio_over = "top_solid_infill_acceleration";
    def->min = 0;
    def->max_literal = { -200, false };
    def->mode = comExpert | comSuSi;
    def->set_default_value(new ConfigOptionFloatOrPercent(0,false));

    def = definition.add("ironing_angle", coFloat, ptFFF);
    def->label = L("Ironing angle");
    def->category = OptionCategory::ironing;
    def->invalidates_step = posInfill;
    def->tooltip = L("Ironing post-process angle."
        "\nIf positive, the ironing will use this angle."
        "\nIf -1, it will use the fill angle."
        "\nIf lower than -1, it will use the fill angle minus this angle.");
    def->sidetext = L("°");
    def->min = -360;
    def->mode = comExpert | comSuSi;
    def->set_default_value(new ConfigOptionFloat(-45));

    def = definition.add("ironing_type", coEnum, ptFFF);
    def->label = L("Ironing Type");
    def->category = OptionCategory::ironing;
    def->invalidates_step = posPrepareInfill;
    def->tooltip = L("Ironing Type");
    def->set_enum<IroningType>({
        { "top",        L("All top surfaces") },
        { "topmost",    L("Topmost surface only") },
        { "solid",      L("All solid surfaces") }
    });
    def->mode = comAdvancedE | comPrusa;
    def->set_default_value(new ConfigOptionEnum<IroningType>(IroningType::TopSurfaces));

    def = definition.add("ironing_flowrate", coPercent, ptFFF);
    def->label = L("Flow rate");
    def->category = OptionCategory::ironing;
    def->invalidates_step = posInfill;
    def->tooltip = L("Percent of a flow rate relative to object's normal layer height."
                " It's the percentage of the layer that will be over-extruded on top to do the ironing.");
    def->sidetext = L("%");
    def->ratio_over = "layer_height";
    def->min = 0;
    def->mode = comExpert | comPrusa;
    def->set_default_value(new ConfigOptionPercent(15));

    def = definition.add("ironing_spacing", coFloatOrPercent, ptFFF);
    def->label = L("Spacing between ironing lines");
    def->category = OptionCategory::ironing;
    def->invalidates_step = posInfill;
    def->tooltip = L("Distance between ironing lines."
                    "\nCan be a % of the nozzle diameter used for ironing.");
    def->sidetext = L("mm or %");
    def->ratio_over = "nozzle_diameter";
    def->min = 0;
    def->mode = comExpert | comPrusa;
    def->set_default_value(new ConfigOptionFloatOrPercent(25, true));

    def = definition.add("ironing_speed", coFloatOrPercent, ptFFF);
    def->label = L("Ironing");
    def->full_label = L("Ironing speed");
    def->category = OptionCategory::ironing;
    def->invalidates_step = psGCodeExport;
    def->tooltip = L("Ironing speed. Used for the ironing pass of the ironing infill pattern, and the post-process infill."
        "\nThis can be expressed as a percentage (for example: 80%) over the Top Solid Infill speed."
        "\nIroning extrusions are ignored from the automatic volumetric speed computation.");
    def->sidetext = L("mm/s");
    def->ratio_over = "top_solid_infill_speed";
    def->min = 0.1;
    def->mode = comExpert | comPrusa;
    def->set_default_value(new ConfigOptionFloatOrPercent(50, true));

    def = definition.add("layer_gcode", coString, ptFFF);
    def->label = L("After layer change G-code");
    def->category = OptionCategory::customgcode;
    def->invalidates_step = psGCodeExport;
    def->tooltip = L("This custom code is inserted at every layer change, right after the Z move "
        "and before the extruder moves to the first layer point. Note that you can use "
        "placeholder variables for all Slic3r settings as well as {layer_num} and {layer_z}.");
    def->cli = "after-layer-gcode|layer-gcode";
    def->multiline = true;
    def->full_width = true;
    def->height = 5;
    def->mode = comExpert | comPrusa;
    def->set_default_value(new ConfigOptionString(""));

    def = definition.add("feature_gcode", coString, ptFFF);
    def->label = L("After extrusion type change in G-code");
    def->category = OptionCategory::customgcode;
    def->invalidates_step = psGCodeExport;
    def->tooltip = L("This custom code is inserted at every extrusion type change."
        "Note that you can use placeholder variables for all Slic3r settings as well as {last_extrusion_role}, {extrusion_role}, {layer_num} and {layer_z}."
        " The 'extrusion_role' strings can take these string values:"
        " { Perimeter, ExternalPerimeter, OverhangPerimeter, InternalInfill, SolidInfill, TopSolidInfill, BridgeInfill, GapFill, Skirt, SupportMaterial, SupportMaterialInterface, WipeTower, Mixed }."
        " Mixed is only used when the role of the extrusion is not unique, not exactly inside another category or not known.");
    def->cli = "feature-gcode";
    def->multiline = true;
    def->full_width = true;
    def->height = 5;
    def->mode = comExpert | comSuSi;
    def->set_default_value(new ConfigOptionString(""));

    // def = definition.add("exact_last_layer_height", coBool);
    // def->label = L("Exact last layer height");
    // def->category = OptionCategory::perimeter;
    // def->tooltip = L("This setting controls the height of last object layers to put the last layer at the exact highest height possible. Experimental.");
    // def->mode = comHidden;
    // def->set_default_value(new ConfigOptionBool(false));

    def = definition.add("remaining_times", coBool, ptFFF);
    def->label = L("Supports remaining times");
    def->category = OptionCategory::firmware;
    def->invalidates_step = psGCodeExport;
    def->tooltip = L("Emit something at 1 minute intervals into the G-code to let the firmware show accurate remaining time.");
    def->mode = comExpert | comPrusa;
    def->set_default_value(new ConfigOptionBool(false));

    def = definition.add("remaining_times_type", coEnum, ptFFF);
    def->label = L("Method");
    def->full_label = L("Supports remaining times method");
    def->category = OptionCategory::firmware;
    def->invalidates_step = psGCodeExport;
    def->tooltip = L("M73: Emit M73 P{percent printed} R{remaining time in minutes} at 1 minute"
        " intervals into the G-code to let the firmware show accurate remaining time."
        " As of now only the Prusa i3 MK3 firmware recognizes M73."
        " Also the i3 MK3 firmware supports M73 Qxx Sxx for the silent mode."
        "\nM117: Send a command to display a message to the printer, this is 'Time Left .h..m..s'." );
    def->mode = comExpert | comSuSi;
    def->set_enum<RemainingTimeType>({
        { "m117", L("M117") },
        { "m73", L("M73") },
        { "m73m117", L("M73 & M117") },
    });
    def->set_default_value(new ConfigOptionEnum<RemainingTimeType>(RemainingTimeType::rtM73));

    def = definition.add("silent_mode", coBool, ptFFF);
    def->label = L("Supports stealth mode");
    def->category = OptionCategory::firmware;
    def->invalidates_step = psGCodeExport;
    def->tooltip = L("The firmware supports stealth mode");
    def->mode = comAdvancedE | comPrusa;
    def->set_default_value(new ConfigOptionBool(false));

    def = definition.add("fan_speedup_time", coFloat, ptFFF);
    def->label = L("Fan startup delay");
    def->category = OptionCategory::firmware;
    def->invalidates_step = psGCodeExport;
    def->tooltip = L("Move the fan start in the past by at least this delay (in seconds, you can use decimals)."
        " It assumes infinite acceleration for this time estimation, and will only take into account G1 and G0 moves."
        "\nIt won't move fan commands from custom gcodes (they act as a sort of 'barrier')."
        "\nIt won't move fan commands into the start gcode if the 'only custom start gcode' is activated."
        "\nUse 0 to deactivate.");
    def->sidetext = L("s");
    def->mode = comAdvancedE | comSuSi;
    def->set_default_value(new ConfigOptionFloat(0));

    def = definition.add("fan_speedup_overhangs", coBool, ptFFF);
    def->label = L("Fan delay only for overhangs");
    def->category = OptionCategory::firmware;
    def->invalidates_step = psGCodeExport;
    def->tooltip = L("Will only take into account the delay for the cooling of overhangs.");
    def->mode = comAdvancedE | comSuSi;
    def->set_default_value(new ConfigOptionBool(true));

    def = definition.add("binary_gcode", coBool, ptFFF);
    def->label = L("Supports binary G-code");
    def->invalidates_step = psGCodeExport;
    def->tooltip = L("Enable, if the firmware supports binary G-code format (bgcode). "
                     "To generate .bgcode files, make sure you have binary G-code enabled in Configuration->Preferences->Other.");
    def->mode = comExpert | comPrusa;
    def->set_default_value(new ConfigOptionBool(false));

    def = definition.add("fan_kickstart", coFloat, ptFFF);
    def->label = L("Fan KickStart time");
    def->category = OptionCategory::firmware;
    def->invalidates_step = psGCodeExport;
    def->tooltip = L("Add a M106 S255 (max speed for fan) for this amount of seconds before going down to the desired speed to kick-start the cooling fan."
                    "\nThis value is used for a 0->100% speedup, it will go down if the delta is lower."
                    "\nSet to 0 to deactivate.");
    def->sidetext = L("s");
    def->min = 0;
    def->mode = comExpert | comSuSi;
    def->set_default_value(new ConfigOptionFloat(0));

    def = definition.add("lift_min", coFloat, ptFFF);
    def->label = L("Min height for travel");
    def->category = OptionCategory::extruders;
    def->invalidates_step = psGCodeExport;
    def->tooltip = L("When an extruder travels to an object (from the start position or from an object to another), the nozzle height is guaranteed to be at least at this value."
        "\nIt's made to ensure the nozzle won't hit clips or things you have on your bed. But be careful to not put a clip in the 'convex shape' of an object."
        "\nSet to 0 to disable.");
    def->sidetext = L("mm");
    def->min = 0;
    def->mode = comExpert | comSuSi;
    def->set_default_value(new ConfigOptionFloat(0));

    def = definition.add("machine_limits_usage", coEnum, ptFFF);
    def->label = L("How to apply limits");
    def->full_label = L("Purpose of Machine Limits");
    def->category = OptionCategory::limits;
    def->tooltip = L("How to apply the Machine Limits."
                    "\n* In every case, they will be used as safeguards: Even if you use a print profile that sets an acceleration of 5000,"
                    " if in your machine limits the acceleration is 4000, the outputted gcode will use the 4000 limit."
                    "\n* You can also use it as a safeguard and to have a better printing time estimate."
                    "\n* You can also use it as a safeguard, to have a better printing time estimate and emit the limits at the beginning of the gcode file, with M201 M202 M203 M204 and M205 commands."
                    " If you want only to write a sub-set, choose the 'for time estimate' option and write your own gcodes in the custom gcode section.");
    def->set_enum<MachineLimitsUsage>({
        { "emit_to_gcode",      L("Also emit limits to G-code") },
        { "time_estimate_only", L("Use also for time estimate") },
        { "limits",             L("Use only as safeguards") },
        { "ignore",             L("Disable") }
    });
    def->mode = comAdvancedE | comPrusa;
    def->set_default_value(new ConfigOptionEnum<MachineLimitsUsage>(MachineLimitsUsage::TimeEstimateOnly));

    {
        struct AxisDefault {
            std::string         name;
            std::vector<double> max_feedrate;
            std::vector<double> max_acceleration;
            std::vector<double> max_jerk;
        };
        std::vector<AxisDefault> axes {
            // name, max_feedrate,  max_acceleration, max_jerk
            { "x", { 500., 200. }, {  9000., 1000. }, { 10. , 10.  } },
            { "y", { 500., 200. }, {  9000., 1000. }, { 10. , 10.  } },
            { "z", {  12.,  12. }, {   500.,  200. }, {  0.2,  0.4 } },
            { "e", { 120., 120. }, { 10000., 5000. }, {  2.5,  2.5 } }
        };
        for (const AxisDefault &axis : axes) {
            std::string axis_upper = boost::to_upper_copy<std::string>(axis.name);
            // Add the machine feedrate limits for XYZE axes. (M203)
            def = definition.add("machine_max_feedrate_" + axis.name, coFloats, ptFFF);
            def->full_label = (boost::format("Maximum feedrate %1%") % axis_upper).str();
            (void)L("Maximum feedrate X");
            (void)L("Maximum feedrate Y");
            (void)L("Maximum feedrate Z");
            (void)L("Maximum feedrate E");
            def->category = OptionCategory::limits;
            def->tooltip  = (boost::format("Maximum feedrate of the %1% axis") % axis_upper).str();
            (void)L("Maximum feedrate of the X axis");
            (void)L("Maximum feedrate of the Y axis");
            (void)L("Maximum feedrate of the Z axis");
            (void)L("Maximum feedrate of the E axis");
            def->sidetext = L("mm/s");
            def->min = 0;
            def->mode = comAdvancedE | comPrusa;
            def->set_default_value(new ConfigOptionFloats(axis.max_feedrate));
            // Add the machine acceleration limits for XYZE axes (M201)
            def = definition.add("machine_max_acceleration_" + axis.name, coFloats, ptFFF);
            def->full_label = (boost::format("Maximum acceleration %1%") % axis_upper).str();
            (void)L("Maximum acceleration X");
            (void)L("Maximum acceleration Y");
            (void)L("Maximum acceleration Z");
            (void)L("Maximum acceleration E");
            def->category = OptionCategory::limits;
            def->tooltip  = (boost::format("Maximum acceleration of the %1% axis") % axis_upper).str();
            (void)L("Maximum acceleration of the X axis");
            (void)L("Maximum acceleration of the Y axis");
            (void)L("Maximum acceleration of the Z axis");
            (void)L("Maximum acceleration of the E axis");
            def->sidetext = L("mm/s²");
            def->min = 0;
            def->mode = comAdvancedE | comPrusa;
            def->set_default_value(new ConfigOptionFloats(axis.max_acceleration));
            // Add the machine jerk limits for XYZE axes (M205)
            def = definition.add("machine_max_jerk_" + axis.name, coFloats, ptFFF);
            def->full_label = (boost::format("Maximum jerk %1%") % axis_upper).str();
            (void)L("Maximum jerk X");
            (void)L("Maximum jerk Y");
            (void)L("Maximum jerk Z");
            (void)L("Maximum jerk E");
            def->category = OptionCategory::limits;
            def->tooltip  = (boost::format("Maximum jerk of the %1% axis") % axis_upper).str();
            (void)L("Maximum jerk of the X axis");
            (void)L("Maximum jerk of the Y axis");
            (void)L("Maximum jerk of the Z axis");
            (void)L("Maximum jerk of the E axis");
            def->sidetext = L("mm/s");
            def->min = 0;
            def->mode = comAdvancedE | comPrusa;
            def->set_default_value(new ConfigOptionFloats(axis.max_jerk));
        }
    }

    // M205 S... [mm/sec]
    def = definition.add("machine_min_extruding_rate", coFloats, ptFFF);
    def->full_label = L("Minimum feedrate when extruding");
    def->category = OptionCategory::limits;
    def->tooltip = L("Minimum feedrate when extruding (M205 S)");
    def->sidetext = L("mm/s");
    def->min = 0;
    def->mode = comAdvancedE | comPrusa;
    def->set_default_value(new ConfigOptionFloats{ 0., 0. });

    // M205 T... [mm/sec]
    def = definition.add("machine_min_travel_rate", coFloats, ptFFF);
    def->full_label = L("Minimum travel feedrate");
    def->category = OptionCategory::limits;
    def->tooltip = L("Minimum travel feedrate (M205 T)");
    def->sidetext = L("mm/s");
    def->min = 0;
    def->mode = comAdvancedE | comPrusa;
    def->set_default_value(new ConfigOptionFloats{ 0., 0. });

    // M204 P... [mm/sec^2]
    def = definition.add("machine_max_acceleration_extruding", coFloats, ptFFF);
    def->full_label = L("Maximum acceleration when extruding");
    def->category = OptionCategory::limits;
    def->tooltip = L("Maximum acceleration when extruding");
    def->sidetext = L("mm/s²");
    def->min = 0;
    def->mode = comAdvancedE | comPrusa;
    def->set_default_value(new ConfigOptionFloats{ 1500., 1250. });

    // M204 R... [mm/sec^2]
    def = definition.add("machine_max_acceleration_retracting", coFloats, ptFFF);
    def->full_label = L("Maximum acceleration when retracting");
    def->category = OptionCategory::limits;
    def->tooltip = L("Maximum acceleration when retracting.\n\n"
                     "Not used for RepRapFirmware, which does not support it.");
    def->sidetext = L("mm/s²");
    def->min = 0;
    def->mode = comAdvancedE | comPrusa;
    def->set_default_value(new ConfigOptionFloats{ 1500., 1250. });

    // M204 T... [mm/sec^2]
    def = definition.add("machine_max_acceleration_travel", coFloats, ptFFF);
    def->full_label = L("Maximum acceleration for travel moves");
    def->category = OptionCategory::limits;
    def->tooltip = L("Maximum acceleration for travel moves.");
    def->sidetext = L("mm/s²");
    def->min = 0;
    def->mode = comAdvancedE | comPrusa;
    def->set_default_value(new ConfigOptionFloats{ 1500., 1250. });

    def = definition.add("max_gcode_per_second", coFloat, ptFFF);
    def->label = L("Maximum G1 per second (Experimental)");
    def->category = OptionCategory::speed;
    def->invalidates_step = psGCodeExport;
    def->tooltip = L("If your firmware stops while printing, it may have its gcode queue full."
        " Set this parameter to merge extrusions into bigger ones to reduce the number of gcode commands the printer has to process each second."
        "\nOn 8bit controlers, a value of 150 is typical."
        "\nNote that reducing your printing speed (at least for the external extrusions) will reduce the number of time this will triggger and so increase quality."
        "\nDisabled if set to 0.");
    def->min = 0;
    def->mode = comExpert | comSuSi;
    def->can_be_disabled = true;
    def->set_default_value(disable_default_option(new ConfigOptionFloat(1500)));

    def = definition.add("max_fan_speed", coInts, ptFFF);
    def->label = L("Max");
    def->full_label = L("Max fan speed");
    def->category = OptionCategory::cooling;
    def->invalidates_step = psGCodeExport;
    def->tooltip = L("This setting represents the maximum speed of your fan, used when the layer print time is Very short.");
    def->sidetext = L("%");
    def->min = 0;
    def->max = 100;
    def->mode = comAdvancedE | comPrusa;
    def->is_vector_extruder = true;
    def->set_default_value(new ConfigOptionInts { 100 });

    def = definition.add("max_layer_height", coFloatsOrPercents, ptFFF);
    def->label = L("Max");
    def->full_label = L("Max layer height");
    def->category = OptionCategory::general;
    def->invalidates_step = posSlice;
    def->tooltip = L("This is the highest printable layer height for this extruder, used to cap "
                   "the variable layer height and support layer height. Maximum recommended layer height "
                   "is 75% of the extrusion width to achieve reasonable inter-layer adhesion. "
                   "\nCan be a % of the nozzle diameter."
                   "\nIf disabled, layer height is limited to 75% of the nozzle diameter.");
    def->sidetext = L("mm or %");
    def->ratio_over = "nozzle_diameter";
    def->min = 0;
    def->max_literal = { 1, true };
    def->mode = comSimpleAE | comPrusa;
    def->is_vector_extruder = true;
    def->can_be_disabled = true;
    def->set_default_value(enable_default_option(new ConfigOptionFloatsOrPercents{ FloatOrPercent{ 75, true} }));

    def = definition.add("max_print_speed", coFloatOrPercent, ptFFF);
    def->label = L("Max Autospeed");
    def->full_label = L("Max print speed for Autospeed");
    def->category = OptionCategory::speed;
    def->invalidates_step = psGCodeExport;
    def->tooltip = L("When setting other speed settings to 0, Slic3r will autocalculate the optimal speed "
        "in order to keep constant extruder pressure. This experimental setting is used "
        "to set the highest print speed you want to allow."
        "\nThis can be expressed as a percentage (for example: 100%) over the machine Max Feedrate for X axis.");
    def->sidetext = L("mm/s or %");
    def->ratio_over = "machine_max_feedrate_x";
    def->min = 1;
    def->mode = comExpert | comPrusa;
    def->set_default_value(new ConfigOptionFloatOrPercent(80, false));

    def = definition.add("max_speed_reduction", coPercents, ptFFF);
    def->label = L("Max speed reduction");
    def->category = OptionCategory::speed;
    def->invalidates_step = psGCodeExport;
    def->tooltip = L("This setting control by how much the speed can be reduced to increase the layer time."
        " It's a maximum reduction, so a lower value makes the minimum speed higher."
        " Set to 90% if you don't want the speed to go below 10% of the current speed."
        "\nSet zero to disable");
    def->sidetext = L("%");
    def->min = 0;
    def->max = 100;
    def->mode = comExpert | comSuSi;
    def->is_vector_extruder = true;
    def->set_default_value(new ConfigOptionPercents{ 90 });

    def = definition.add("max_volumetric_speed", coFloat, ptFFF);
    def->label = L("Maximum flow for Autospeed");
    def->full_label = L("Maximum Volumetric print speed for Autospeed");
    def->category = OptionCategory::extruders;
    def->invalidates_step = psGCodeExport;
    def->tooltip = L("This setting allows you to set the maximum flowrate for your print, and so cap the desired flow rate for the autospeed algorithm."
        " The autospeed tries to keep a constant feedrate for the entire object, and so can lower the volumetric speed for some features."
        "\nThe autospeed is only enable on speed fields that have a value of 0. If a speed field is a % of a 0 field, then it will be a % of the value it should have got from the autospeed."
        "\nIf this field is set to 0, then there is no autospeed nor maximum flowrate. If a speed value i still set to 0, it will get the max speed allwoed by the printer.");
    def->sidetext = L("mm³/s");
    def->min = 0;
    def->mode = comExpert | comPrusa;
    def->set_default_value(new ConfigOptionFloat(0));

    def = definition.add("max_volumetric_extrusion_rate_slope_positive", coFloat, ptFFF);
    def->label = L("Max volumetric slope positive");
    def->invalidates_step = psGCodeExport;
    def->tooltip = L("This experimental setting is used to limit the speed of change in extrusion rate "
                       "for a transition from lower speed to higher speed. "
                   "A value of 1.8 mm³/s² ensures, that a change from the extrusion rate "
                   "of 1.8 mm³/s (0.45mm extrusion width, 0.2mm extrusion height, feedrate 20 mm/s) "
                   "to 5.4 mm³/s (feedrate 60 mm/s) will take at least 2 seconds.");
    def->sidetext = L("mm³/s²");
    def->min = 0;
    def->mode = comExpert | comPrusa;
    def->set_default_value(new ConfigOptionFloat(0));

    def = definition.add("max_volumetric_extrusion_rate_slope_negative", coFloat, ptFFF);
    def->label = L("Max volumetric slope negative");
    def->invalidates_step = psGCodeExport;
    def->tooltip = L("This experimental setting is used to limit the speed of change in extrusion rate "
                       "for a transition from higher speed to lower speed. "
                   "A value of 1.8 mm³/s² ensures, that a change from the extrusion rate "
                   "of 5.4 mm³/s (0.45 mm extrusion width, 0.2 mm extrusion height, feedrate 60 mm/s) "
                   "to 1.8 mm³/s (feedrate 20 mm/s) will take at least 2 seconds.");
    def->sidetext = L("mm³/s²");
    def->min = 0;
    def->mode = comExpert | comPrusa;
    def->set_default_value(new ConfigOptionFloat(0));

#if 0
    // replaced by fan_printer_min_speed, to remove !! @DEPRECATED
    def = definition.add("min_fan_speed", coInts, ptFFF);
    def->label = L("Default fan speed");
    def->full_label = L("Default fan speed");
    def->category = OptionCategory::cooling;
    def->tooltip = L("This setting represents the base fan speed this filament needs, or at least the minimum PWM your fan needs to work.");
    def->sidetext = L("%");
    def->min = 0;
    def->max = 100;
    def->mode = comSimpleAE | comPrusa;
    def->is_vector_extruder = true;
    def->set_default_value(new ConfigOptionInts{ 35 });
#endif

    def = definition.add("fan_percentage", coBool, ptFFF);
    def->label = L("Fan PWM from 0-100");
    def->category = OptionCategory::output;
    def->invalidates_step = psGCodeExport;
    def->tooltip = L("Set this if your printer uses control values from 0-100 instead of 0-255.");
    def->cli = "fan-percentage";
    def->mode = comAdvancedE | comSuSi;
    def->set_default_value(new ConfigOptionBool(false));

    def = definition.add("min_layer_height", coFloatsOrPercents, ptFFF);
    def->label = L("Min");
    def->full_label = L("Min layer height");
    def->category = OptionCategory::extruders;
    def->invalidates_step = posSlice;
    def->tooltip = L("This is the lowest printable layer height for this extruder and limits "
                    "the resolution for variable layer height. Typical values are between 0.05 mm and 0.1 mm."
                    "\nCan be a % of the nozzle diameter.");
    def->sidetext = L("mm or %");
    def->ratio_over = "nozzle_diameter";
    def->min = 0;
    def->max_literal = { 5, false };
    def->mode = comSimpleAE | comPrusa;
    def->is_vector_extruder = true;
    def->set_default_value(new ConfigOptionFloatsOrPercents{ FloatOrPercent{ 5, true} });

    def = definition.add("min_width_top_surface", coFloatOrPercent, ptFFF);
    def->label = L("Minimum top width for infill");
    def->category = OptionCategory::speed;
    def->invalidates_step = posPerimeters;
    def->tooltip = L("If a top surface has to be printed and it's partially covered by another layer, it won't be considered at a top layer where its width is below this value."
        " This can be useful to not let the 'one perimeter on top' trigger on surface that should be covered only by perimeters."
        " This value can be a mm or a % of the perimeter extrusion width."
        "\nWarning: If enabled, artifacts can be created is you have some thin features on the next layer, like letters. Set this setting to 0 to remove these artifacts.");
    def->sidetext = L("mm or %");
    def->ratio_over = "perimeter_extrusion_width";
    def->min = 0;
    def->max_literal = { 15, false };
    def->mode = comExpert | comSuSi;
    def->set_default_value(new ConfigOptionFloatOrPercent(100, true));

    def = definition.add("min_print_speed", coFloats, ptFFF);
    def->label = L("Min print speed");
    def->category = OptionCategory::speed;
    def->invalidates_step = psGCodeExport;
    def->tooltip = L("Slic3r will never scale the speed below this one.");
    def->sidetext = L("mm/s");
    def->min = 0;
    def->mode = comExpert | comPrusa;
    def->is_vector_extruder = true;
    def->set_default_value(new ConfigOptionFloats{ 10. });

    def = definition.add("min_skirt_length", coFloat, ptFFF);
    def->label = L("Minimal filament extrusion length");
    def->category = OptionCategory::skirtBrim;
    def->invalidates_step = psSkirtBrim;
    def->tooltip = L("Generate no less than the number of skirt loops required to consume "
                   "the specified amount of filament on the bottom layer. For multi-extruder machines, "
                   "this minimum applies to each extruder.");
    def->sidetext = L("mm");
    def->min = 0;
    def->mode = comExpert | comPrusa;
    def->set_default_value(new ConfigOptionFloat(0));

    def = definition.add("model_precision", coFloat, ptFFF);
    def->label = L("Model rounding precision");
    def->full_label = L("Model rounding precision");
    def->category = OptionCategory::slicing;
    def->invalidates_step = posSlice;
    def->tooltip = L("This is the rounding error of the input object."
        " It's used to align points that should be in the same line."
        "\nSet zero to disable.");
    def->sidetext = L("mm");
    def->min = 0;
    def->precision = 8;
    def->mode = comAdvancedE | comSuSi;
    def->set_default_value(new ConfigOptionFloat(0.0001));

    def = definition.add("notes", coString, ptFFF);
    def->label = L("Configuration notes");
    def->category = OptionCategory::notes;
    def->invalidates_step = psGCodeExport;
    def->tooltip = L("Here you can put your personal notes. This text will be added to the G-code "
                   "header comments.");
    def->multiline = true;
    def->full_width = true;
    def->height = 13;
    def->mode = comAdvancedE | comPrusa;
    def->set_default_value(new ConfigOptionString(""));

    def = definition.add("nozzle_diameter", coFloats, ptFFF);
    def->label = L("Nozzle diameter");
    def->category = OptionCategory::extruders;
    def->invalidates_step = posSlice;
    def->tooltip = L("This is the diameter of your extruder nozzle (for example: 0.5, 0.35 etc.)");
    def->sidetext = L("mm");
    def->mode = comAdvancedE | comPrusa;
    def->is_vector_extruder = true;
    def->set_default_value(new ConfigOptionFloats{ 0.4 });

    def = definition.add("host_type", coEnum, ptFFF);
    def->label = L("Host Type");
    def->category = OptionCategory::general;
    def->tooltip = L("Slic3r can upload G-code files to a printer host. This field must contain "
                   "the kind of the host."
                   "\nPrusaLink is only available for prusa printer.");
    def->set_enum<PrintHostType>({
        { "prusalink",      "PrusaLink" },
        { "prusaconnect",   "PrusaConnect" },
        { "octoprint",      "OctoPrint" },
        { "moonraker",      "Klipper (via Moonraker)" },
        { "duet",           "Duet" },
        { "flashair",       "FlashAir" },
        { "astrobox",       "AstroBox" },
        { "repetier",       "Repetier" },
        { "klipper",        "Klipper" },
        { "mpmdv2",         "MPMDv2" },
        { "mks",            "MKS" },
        { "monoprice",      "Monoprice lcd" },
    });
    def->mode = comAdvancedE | comPrusa;
    def->cli = ConfigOptionDef::nocli;
    def->set_default_value(new ConfigOptionEnum<PrintHostType>(htPrusaLink));

    def = definition.add("print_custom_variables", coString, ptFFF);
    def->label = L("Custom variables");
    def->full_label = L("Custom Print variables");
    def->category = OptionCategory::filament;
    def->invalidates_step = psGCodeExport;
    def->tooltip = L("You can add data accessible to custom-gcode macros."
        "\nEach line can define one variable."
        "\nThe format is 'variable_name=value'. the variable name should only have [a-zA-Z0-9] characters or '_'."
        "\nA value that can be parsed as a int or float will be available as a numeric value."
        "\nA value that is enclosed by double-quotes will be available as a string (without the quotes)"
        "\nA value that only takes values as 'true' or 'false' will be a boolean)"
        "\nEvery other value will be parsed as a string as-is."
        "\nAdvice: before using a variable, it's safer to use the function 'default_XXX(variable_name, default_value)'"
        " (enclosed in bracket as it's a script) in case it's not set. You can replace XXX by 'int' 'bool' 'double' 'string'.");
    def->multiline = true;
    def->full_width = true;
    def->height = 13;
    def->mode = comAdvancedE | comSuSi;
    def->set_default_value(new ConfigOptionString{ "" });

    def = definition.add("object_gcode", coString, ptFFF);
    def->label = L("Per object G-code");
    def->category = OptionCategory::advanced;
    def->invalidates_step = psGCodeExport;
    def->tooltip = L("This code is inserted each layer, when the object began to print (just after the label if any)."
                     " It's main advantage is when you use it as a object modifier (right click on a model)."
                     "\nSpecial variables: 'layer_num','layer_z'");
    def->multiline = true;
    def->full_width = true;
    def->height = 10;
    def->mode = comExpert | comSuSi;
    def->set_default_value(new ConfigOptionString(""));

    def = definition.add("only_one_perimeter_first_layer", coBool, ptFFF);
    def->label = L("Only one perimeter on First layer");
    def->category = OptionCategory::perimeter;
    def->invalidates_step = posPerimeters;
    def->tooltip = L("Use only one perimeter on first layer, to give more space to the top infill pattern.");
    def->mode = comAdvancedE | comSuSi;
    def->set_default_value(new ConfigOptionBool(false));

    def = definition.add("only_one_perimeter_top", coBool, ptFFF);
    def->label = L("Only one perimeter on Top surfaces");
    def->category = OptionCategory::perimeter;
    def->invalidates_step = posPerimeters;
    def->tooltip = L("Use only one perimeter on flat top surface, to give more space to the top infill pattern.");
    def->mode = comSimpleAE | comSuSi;
    def->set_default_value(new ConfigOptionBool(true));

    def = definition.add("only_one_perimeter_top_other_algo", coBool, ptFFF);
    def->label = L("Only one peri - other algo");
    def->category = OptionCategory::perimeter;
    def->invalidates_step = posPerimeters;
    def->tooltip = L("If you have some problem with the 'Only one perimeter on Top surfaces' option, you can try to activate this on the problematic layer.");
    def->mode = comHidden;
    def->set_default_value(new ConfigOptionBool(false));

    def = definition.add("only_retract_when_crossing_perimeters", coBool, ptFFF);
    def->label = L("Only retract when crossing perimeters");
    def->category = OptionCategory::extruders;
    def->invalidates_step = psGCodeExport;
    def->tooltip = L("Disables retraction when the travel path does not exceed the upper layer's perimeters "
        "(and thus any ooze will probably be invisible).");
    def->mode = comExpert | comPrusa;
    def->set_default_value(new ConfigOptionBool(false));

    def = definition.add("ooze_prevention", coBool, ptFFF);
    def->label = L("Enable");
    def->invalidates_step = psSkirtBrim;
    // TRN PrintSettings: Enable ooze prevention
    def->tooltip = L("This option will drop the temperature of the inactive extruders to prevent oozing.");
    def->mode = comExpert | comPrusa;
    def->set_default_value(new ConfigOptionBool(false));

    def = definition.add("output_filename_format", coString, ptFFF);
    def->label = L("Output filename format");
    def->category = OptionCategory::output;
    def->invalidates_step = psGCodeExport;
    def->tooltip = L("You can use all configuration options as variables inside this template. "
                   "For example: {layer_height}, {fill_density} etc. You can also use {timestamp}, "
                   "{year}, {month}, {day}, {hour}, {minute}, {second}, {version}, {input_filename}, "
                   "{input_filename_base}, {default_output_extension}.");
    def->full_width = true;
    def->mode = comExpert | comPrusa;
    def->set_default_value(new ConfigOptionString("{input_filename_base}.gcode"));

    def = definition.add("overhangs", coBool, ptFFF);
    def->label = L("Detect overhangs");
    def->category = OptionCategory::perimeter;
    def->invalidates_step = posPerimeters;
    def->tooltip = L("Allow to set a specific speed, fan speed and flow (if enabled) for overhangs (unsupported perimeters)");
    def->mode = comSimpleAE | comPrusa;
    def->set_default_value(new ConfigOptionBool(true));

    def = definition.add("overhangs_acceleration", coFloatOrPercent, ptFFF);
    def->label = L("Overhangs");
    def->full_label = L("Overhang acceleration");
    def->category = OptionCategory::speed;
    def->invalidates_step = psGCodeExport;
    def->tooltip = L("This is the acceleration your printer will use for overhangs."
                "\nCan be a % of the bridge acceleration"
                "\nSet zero to to use bridge acceleration for overhangs.");
    def->sidetext = L("mm/s² or %");
    def->ratio_over = "bridge_acceleration";
    def->min = 0;
    def->max_literal = { -200, false };
    def->mode = comExpert | comSuSi;
    def->set_default_value(new ConfigOptionFloatOrPercent(0,false));

    def             = definition.add("overhangs_dynamic_fan_speed", coGraphs, ptFFF);
    def->label      = L("Dynamic overhang speeds");
    def->category   = OptionCategory::speed;
    def->invalidates_step = psGCodeExport;
    def->tooltip    = L("This setting can only works correctly if dynamic speed is also enabled (overhangs_dynamic_fan_speed)."
        "\nOverhang size is expressed as a percentage of overlap of the extrusion with the previous layer: "
        "100% would be full overlap (no overhang), while 0% represents full overhang (floating extrusion, bridge)."
        "\nFan speeds for overhang sizes in between are calculated via linear interpolation."
        "\nIf enabled, overhangs_fan_speed is disabled, as the fan speed for full overhang is used.");
    def->sidetext   = L("%");
    def->is_vector_extruder = true;
    def->can_be_disabled = true;
    def->mode       = comExpert | comPrusa;
    def->set_default_value(disable_default_option(new ConfigOptionGraphs({GraphData(0,5, GraphData::GraphType::LINEAR,
        {{0,100},{25,80},{50,60},{75,40},{100,20}}
    )})));
    def->graph_settings = std::make_shared<GraphSettings>();
    def->graph_settings->title       = L("Overhangs fan speed by % of overlap");
    def->graph_settings->description = L("Choose the Overhangs maximum fan speed for each percentage of overlap with the layer below."
        "If the current fan speed (from perimeter, external, of default) is higher, then this setting won't slow the fan."
        "\n100% overlap is when the extrusion is fully on top of the previous layer's extrusion."
        "\n0% overlap is when the extrusion centerline is at a distance of 'overhangs threshold for speed'(overhangs_bridge_threshold)"
        "\nfrom the nearest extrusion of the previous layer.");
    def->graph_settings->x_label     = L("overlap % with previous layer");
    def->graph_settings->y_label     = L("Fan speed (%)");
    def->graph_settings->null_label  = L("No fan speed");
    def->graph_settings->label_min_x = L("");
    def->graph_settings->label_max_x = L("");
    def->graph_settings->label_min_y = L("");
    def->graph_settings->label_max_y = L("");
    def->graph_settings->min_x       = 0;
    def->graph_settings->max_x       = 100;
    def->graph_settings->step_x      = 1.;
    def->graph_settings->min_y       = 0;
    def->graph_settings->max_y       = 100;
    def->graph_settings->step_y      = 1.;
    def->graph_settings->allowed_types = {GraphData::GraphType::LINEAR, GraphData::GraphType::SQUARE, GraphData::GraphType::SPLINE};

    def             = definition.add("overhangs_dynamic_flow", coGraph, ptFFF);
    def->label      = L("Dynamic overhang flow");
    def->category   = OptionCategory::speed;
    def->invalidates_step = posPerimeters;
    def->tooltip    = L("Overhang size is expressed as a percentage of overlap of the extrusion with the previous layer:"
                        " 100% would be full overlap (no overhang), while 0% represents full overhang (floating extrusion, bridge)"
                        " defined by the 'overhang flow threshold' (overhang_width) setting."
                        " The flow can vary between the (external) perimeter flow and the overhang flow.");
    def->can_be_disabled = true;
    def->mode       = comExpert | comSuSi;
    def->set_default_value(enable_default_option(new ConfigOptionGraph(GraphData(0,5, GraphData::GraphType::LINEAR,
        {{0,0},{25,0},{50,15},{75,50},{100,100}}
    ))));
    def->graph_settings = std::make_shared<GraphSettings>();
    def->graph_settings->title       = L("Overhangs flow ratio by % of overlap");
    def->graph_settings->description = L("Choose the Overhangs flow for each percentage of overlap with the layer below."
        "\nThe flow is a percentage ratio between perimeter / external perimeter flow (for 100% overlap - no overhang) and overhangs flow (for overhangs)."
        "\n'no overhangs' (100% overlap) is when the extrusion is fully on top of the previous layer's extrusion."
        "\n'overhang' is when the extrusion centerline is at a distance of 'overhangs threshold for flow'"
        "\n(overhangs_width) from the nearest extrusion of the previous layer.");
    def->graph_settings->x_label     = L("overlap % with previous layer");
    def->graph_settings->y_label     = L("Speed ratio (%)");
    def->graph_settings->null_label  = L("Uses overhangs speed");
    def->graph_settings->label_min_x = L("no overhangs");
    def->graph_settings->label_max_x = L("Overhang");
    def->graph_settings->label_min_y = L("Perimeter Flow");
    def->graph_settings->label_max_y = L("Overhang Flow");
    def->graph_settings->min_x       = 100;
    def->graph_settings->max_x       = 0;
    def->graph_settings->max_x       = 100;
    def->graph_settings->step_x      = 1.;
    def->graph_settings->min_y       = 0;
    def->graph_settings->max_y       = 100;
    def->graph_settings->step_y      = 1.;
    def->graph_settings->allowed_types = {GraphData::GraphType::LINEAR, GraphData::GraphType::SQUARE, GraphData::GraphType::SPLINE};

    def             = definition.add("overhangs_dynamic_speed", coGraph, ptFFF);
    def->label      = L("Dynamic overhang speeds");
    def->category   = OptionCategory::speed;
    def->invalidates_step = posPerimeters;
    def->tooltip    = L("Overhang size is expressed as a percentage of overlap of the extrusion with the previous layer:"
                        " 100% would be full overlap (no overhang), while 0% represents full overhang (floating extrusion, bridge)"
                        " defined by the 'overhang speed threshold' (overhangs_width_speed) setting."
                        " Speeds for overhang sizes in between are calculated via linear interpolation,"
                        " as a percentage between the overhang speed and the (external) perimeter speed."
                        "\nNote that the speeds generated to gcode will never exceed the max volumetric speed value.");
    def->sidetext   = L("mm/s");
    def->can_be_disabled = true;
    def->mode       = comExpert | comPrusa;
    def->set_default_value(enable_default_option(new ConfigOptionGraph(GraphData(0,5, GraphData::GraphType::LINEAR,
        {{0,0},{25,10},{50,40},{75,70},{100,100}}
    ))));
    def->graph_settings = std::make_shared<GraphSettings>();
    def->graph_settings->title       = L("Overhangs speed ratio by % of overlap");
    def->graph_settings->description = L("Choose the Overhangs speed for each percentage of overlap with the layer below."
        "\nThe speed is a percentage ratio between overhangs speed (for 0% overlap) and"
        "\nperimeter / external perimeter speed (for 100% overlap)."
        "\n100% overlap is when the extrusion is fully on top of the previous layer's extrusion."
        "\n0% overlap is when the extrusion centerline is at a distance of 'overhangs threshold for speed'"
        "\n(overhangs_width_speed) from the nearest extrusion of the previous layer.");
    def->graph_settings->x_label     = L("overlap % with previous layer");
    def->graph_settings->y_label     = L("Speed ratio (%)");
    def->graph_settings->null_label  = L("Uses overhangs speed");
    def->graph_settings->label_min_x = L("Overhang");
    def->graph_settings->label_max_x = L("No overhangs");
    def->graph_settings->label_min_y = L("Overhang Speed");
    def->graph_settings->label_max_y = L("Perimeter Speed");
    def->graph_settings->min_x       = 0;
    def->graph_settings->max_x       = 100;
    def->graph_settings->max_x       = 100;
    def->graph_settings->step_x      = 1.;
    def->graph_settings->min_y       = 0;
    def->graph_settings->max_y       = 100;
    def->graph_settings->step_y      = 1.;
    def->graph_settings->allowed_types = {GraphData::GraphType::LINEAR, GraphData::GraphType::SQUARE, GraphData::GraphType::SPLINE};

    def = definition.add("overhangs_extrusion_spacing", coFloatOrPercent, ptFFF);
    def->label = L("Overhangs spacing");
    def->full_label = L("Overhangs extrusion spacing");
    def->category = OptionCategory::width;
    def->invalidates_step = posPerimeters;
    def->tooltip = L("Set this to a non-zero value to set a manual extrusion width for perimeters. "
        "This setting change the distance between two overhang lines, but it not affect the cross-section of the extrusion unlike similar settings"
        ", the bridge flow is set separately."
        "If left zero, the distance between two overhang extrusion lines will be the same as that of the (external) perimeters."
        "If expressed as percentage (for example 105%) it will be computed over (current) nozzle diameter."
        "You may want to have a smaller value than for perimeter to ensure the next overhang can stick to the previous one.");
    def->sidetext = L("mm or %");
    def->ratio_over = "nozzle_diameter";
    def->min = 0;
    def->max = 1000;
    def->max_literal = { 10, true };
    def->precision = 6;
    def->mode = comAdvancedE | comSuSi;
    def->set_default_value((new ConfigOptionFloatOrPercent(0, false)));

    def = definition.add("overhangs_fan_speed", coInts, ptFFF);
    def->label = L("Overhangs Perimeter fan speed");
    def->category = OptionCategory::cooling;
    def->invalidates_step = psGCodeExport;
    def->tooltip = L("This fan speed is enforced during all Overhang Perimeter moves"
        "\nIf disabled, the previous (perimeter) fan speed will be used."
        "\nCan be overridden by disable_fan_first_layers and increased by low layer time.");
    def->sidetext = L("%");
    def->min = 0;
    def->max = 100;
    def->mode = comAdvancedE | comSuSi;
    def->is_vector_extruder = true;
    def->can_be_disabled = true;
    def->set_default_value(disable_default_option(new ConfigOptionInts({ 100 })));

    def = definition.add("overhangs_flow_ratio", coPercent, ptFFF);
    def->label = L("Overhangs flow ratio");
    def->sidetext = L("%");
    def->category = OptionCategory::width;
    def->tooltip = L("This factor affects the amount of plastic for overhangs. "
                   "You can increase it to prevent the nozzle to pull the extrudates,"
                    " to have better corners but it will make straight bridging to sag more."
                   "\nYou should experiment with cooling (use a strong fan) before tweaking this."
                   "\nFor reference, the default bridge flow is :"
                    "\n * When using the 'nozzle diameter' as bridge type: (in mm3/mm): (nozzle diameter) * (nozzle diameter) * PI/4"
                    "\n * When using the 'layer height' as bridge type: (in mm3/mm): (layer height) * (layer height) * PI/4"
                    "\n * When using the 'current flow' as bridge type: depends of the current extrusion.");
    def->min = 2;
    def->max = 1000;
    def->can_be_disabled = true;
    def->mode = comAdvancedE | comSuSi;
    def->set_default_value(enable_default_option(new ConfigOptionPercent(100)));

    def = definition.add("overhangs_speed", coFloatOrPercent, ptFFF);
    def->label = L("Overhangs");
    def->full_label = L("Overhangs speed");
    def->category = OptionCategory::speed;
    def->invalidates_step = psGCodeExport;
    def->tooltip = L("Speed for printing overhangs."
        "\nCan be a % of the bridge infill speed."
        "\nSet zero to use autospeed for this feature.");
    def->sidetext = L("mm/s");
    def->ratio_over = "bridge_speed";
    def->min = 0;
    def->mode = comExpert | comSuSi;
    def->set_default_value(new ConfigOptionFloatOrPercent(100, true));

    def = definition.add("overhangs_speed_enforce", coInt, ptFFF);
    def->label = L("Enforce overhangs speed");
    def->full_label = L("Enforce overhangs speed");
    def->category = OptionCategory::speed;
    def->invalidates_step = posPerimeters;
    def->tooltip = L("Set the speed of the full perimeters to the overhang speed, and also the next one(s) if any."
                "\nSet to 0 to disable."
                "\nSet to 1 to set the overhang speed to the full perimeter if there is any overhang detected inside it."
                "\nSet to more than 1 to also set the overhang speed to the next perimeter(s) (only in classic mode)."
                );
    def->sidetext = L("perimeters");
    def->min = 0;
    def->mode = comExpert | comSuSi;
    def->set_default_value(new ConfigOptionInt(0));

    def = definition.add("overhangs_type", coEnum, ptFFF);
    def->label = L("Overhangs flow baseline");
    def->category = OptionCategory::width;
    def->tooltip = L(
        "This setting allow you to choose the base for the overhang flow compute, the result will be multiplied by the "
        "overhang flow to have the final result."
        "\nAn overhang is a perimeter extrusion with nothing under it to flatten it, and so it can't have a 'rectangle' shape "
        "but a circle one."
        "\n * The default way to compute an overhang flow is to use the nozzle diameter as the diameter of the "
        "extrusion cross-section. It shouldn't be higher than that to prevent sagging."
        "\n * A second way to compute an overhang flow is to use the current layer height, so it shouldn't protrude "
        "below it. Note that may create too thin extrusions and so a bad overhang quality."
        "\n * A Third way to compute a overhang flow is to continue to use the current flow/section (mm3 per mm). If "
        "there is no current flow, it will use the external perimeter one."
        " To use if you have some difficulties with the big flow changes from perimeter flow to overhang "
        "flow and vice-versa, the overhang flow ratio let you compensate for the change in speed."
        " \nThe preview will display the expected shape of the overhang extrusion (cylinder), don't expect a magical "
        "thick and solid air to flatten the extrusion magically.");
    def->sidetext = L("%");
    def->set_enum<BridgeType>({
        {"nozzle", L("Nozzle diameter")},
        {"height", L("Layer height")},
        {"flow", L("Keep current flow")},
    });
    def->mode = comAdvancedE | comSuSi;
    def->set_default_value(new ConfigOptionEnum<BridgeType>{BridgeType::btFromNozzle});

    def = definition.add("overhangs_width", coFloatOrPercent, ptFFF);
    def->label = L("Overhangs flow threshold");
    def->category = OptionCategory::width;
    def->invalidates_step = posPerimeters;
    def->tooltip = L("Minimum unsupported width for an extrusion to apply the overhang flow to it."
        "\nCan be in mm or in a % of the nozzle diameter."
        "\nIf dynamic flow is used, then the dynamic flow will be computed between 0% unsupported (100% overlap) and this threshold.");
    def->sidetext = L("mm or %");
    def->ratio_over = "nozzle_diameter";
    def->min = 0;
    def->max_literal = { 10, true };
    def->mode = comExpert | comSuSi;
    def->set_default_value(new ConfigOptionFloatOrPercent(100, true));
    
    def = definition.add("overhangs_width_speed", coFloatOrPercent, ptFFF);
    def->label = L("Overhangs speed threshold");
    def->category = OptionCategory::speed;
    def->invalidates_step = posPerimeters;
    def->tooltip = L("Minimum unsupported width for an extrusion to apply the overhang speed & fan speed to it."
        "\nCan be in mm or in a % of the nozzle diameter."
        "\nIf dynamic speed is used, then the dynamic speed will be computed between 0% unsupported (100% overlap) and this threshold.");
    def->sidetext = L("mm or %");
    def->ratio_over = "nozzle_diameter";
    def->min = 0;
    def->can_be_disabled = true;
    def->mode = comExpert | comSuSi;
    def->set_default_value(disable_default_option(new ConfigOptionFloatOrPercent(100, true)));

    def = definition.add("overhangs_reverse", coBool, ptFFF);
    def->label = L("Reverse on even");
    def->full_label = L("Overhang reversal on even layers");
    def->category = OptionCategory::perimeter;
    def->invalidates_step = posPerimeters;
    def->tooltip = L("Extrude perimeters that have an overhanging part in the reverse direction on even layers (not on odd layers like the first one)."
        " This alternating pattern can significantly improve steep overhangs."
        "\n!! this is a very slow algorithm !!");
    def->mode = comAdvancedE | comSuSi;
    def->set_default_value(new ConfigOptionBool(false));

    def = definition.add("overhangs_reverse_threshold", coFloatOrPercent, ptFFF);
    def->label = L("Reverse threshold");
    def->full_label = L("Overhang reversal threshold");
    def->category = OptionCategory::perimeter;
    def->invalidates_step = posPerimeters;
    def->tooltip = L("Number of mm the overhang need to be for the reversal to be considered useful. Can be a % of the perimeter width.");
    def->ratio_over = "perimeter_extrusion_width";
    def->min = 0;
    def->max_literal = { 20, false };
    def->mode = comAdvancedE | comSuSi;
    def->set_default_value(new ConfigOptionFloatOrPercent(250, true));

    def = definition.add("no_perimeter_unsupported_algo", coEnum, ptFFF);
    def->label = L("No perimeters on bridge areas");
    def->category = OptionCategory::perimeter;
    def->invalidates_step = posPerimeters;
    def->tooltip = L("Experimental option to remove perimeters where there is nothing under them and where a bridged infill should be better. "
        "\n * Remove perimeters: remove the unsupported perimeters, leave the bridge area as-is."
        "\n * Keep only bridges: remove the perimeters in the bridge areas, keep only bridges that end in solid area."
        "\n * Keep bridges and overhangs: remove the unsupported perimeters, keep only bridges that end in solid area, fill the rest with overhang perimeters+bridges."
        "\n * Fill the voids with bridges: remove the unsupported perimeters, draw bridges over the whole hole.*"
        " !! this one can escalate to problems with overhangs shaped like  /\\, so you should use it only on one layer at a time via the height-range modifier!"
        "\n!!Computationally intensive!!. ");
    def->set_enum<NoPerimeterUnsupportedAlgo>({
        { "none",             "Disabled" },
        { "noperi",           "Remove perimeters" },
        { "bridges",          "Keep only bridges" },
        { "bridgesoverhangs", "Keep bridges and overhangs" },
        { "filled",           "Fill the voids with bridges" },
    });
    def->mode = comAdvancedE | comSuSi;
    def->set_default_value(new ConfigOptionEnum<NoPerimeterUnsupportedAlgo>(npuaNone));

    def = definition.add("parking_pos_retraction", coFloat, ptFFF);
    def->label = L("Filament parking position");
    def->invalidates_step = psSkirtBrim;
    def->tooltip = L("Distance of the extruder tip from the position where the filament is parked "
                      "when unloaded. This should match the value in printer firmware. ");
    def->sidetext = L("mm");
    def->min = 0;
    def->mode = comAdvancedE | comPrusa;
    def->set_default_value(new ConfigOptionFloat(92.));

    def = definition.add("extra_loading_move", coFloat, ptFFF);
    def->label = L("Extra loading distance");
    def->invalidates_step = psSkirtBrim;
    def->tooltip = L("When set to zero, the distance the filament is moved from parking position during load "
                      "is exactly the same as it was moved back during unload. When positive, it is loaded further, "
                      " if negative, the loading move is shorter than unloading. ");
    def->sidetext = L("mm");
    def->mode = comAdvancedE | comPrusa;
    def->set_default_value(new ConfigOptionFloat(-2.));

    def = definition.add("perimeter_acceleration", coFloatOrPercent, ptFFF);
    def->label = L("Internal");
    def->full_label = L("Internal Perimeter acceleration");
    def->category = OptionCategory::speed;
    def->invalidates_step = psGCodeExport;
    def->tooltip = L("This is the acceleration your printer will use for internal perimeters. "
                "\nCan be a % of the default acceleration"
                "\nSet zero to use default acceleration for internal perimeters.");
    def->sidetext = L("mm/s² or %");
    def->ratio_over = "default_acceleration";
    def->min = 0;
    def->max_literal = { -200, false };
    def->mode = comAdvancedE | comPrusa;
    def->set_default_value(new ConfigOptionFloatOrPercent(0,false));

    def = definition.add("perimeter_bonding", coPercent, ptFFF);
    def->label = L("Better bonding");
    def->full_label = L("Perimeter bonding");
    def->category = OptionCategory::perimeter;
    def->invalidates_step = posPerimeters;
    def->tooltip = L("This setting may slightly degrade the quality of your external perimeter, in exchange for a better bonding between perimeters."
        "Use it if you have great difficulties with perimeter bonding, for example with high temperature filaments."
        "\nThis percentage is the % of overlap between perimeters, a bit like perimeter_overlap and external_perimeter_overlap, but in reverse."
        " You have to set perimeter_overlap and external_perimeter_overlap to 100%, or this setting has no effect."
        " 0: no effect, 50%: half of the nozzle will be over an already extruded perimeter while extruding a new one"
        ", unless it's an external one)."
        "\nNote: it needs the external and perimeter overlap to be at 100% and to print the external perimeter first.");
    def->sidetext = L("%");
    def->min = 0;
    def->max = 50;
    def->mode = comExpert | comSuSi;
    def->set_default_value(new ConfigOptionPercent(0));

    def = definition.add("perimeter_extruder", coInt, ptFFF);
    def->label = L("Perimeter extruder");
    def->category = OptionCategory::extruders;
    def->invalidates_step = posSlice;
    def->tooltip = L("The extruder to use when printing perimeters and brim. First extruder is 1.");
    def->aliases = { "perimeters_extruder" };
    def->min = 1;
    def->mode = comAdvancedE | comPrusa;
    def->set_default_value(new ConfigOptionInt(1));

    def = definition.add("perimeter_extrusion_width", coFloatOrPercent, ptFFF);
    def->label = L("Perimeters");
    def->full_label = L("Perimeter width");
    def->category = OptionCategory::width;
    def->invalidates_step = posPerimeters;
    def->tooltip = L("Set this to a non-zero value to set a manual extrusion width for perimeters. "
        "You may want to use thinner extrudates to get more accurate surfaces. "
        "If left zero, default extrusion width will be used if set, otherwise 1.125 x nozzle diameter will be used. "
        "If expressed as percentage (for example 105%) it will be computed over nozzle diameter."
        "\nYou can set either 'Spacing', or 'Width'; the other will be calculated, using the perimeter 'Overlap' percentages and default layer height.");
    def->sidetext = L("mm or %");
    def->aliases = { "perimeters_extrusion_width" };
    def->ratio_over = "nozzle_diameter";
    def->min = 0;
    def->max = 1000;
    def->max_literal = { 10, true };
    def->precision = 6;
    def->can_phony = true;
    def->mode = comAdvancedE | comPrusa;
    def->set_default_value((new ConfigOptionFloatOrPercent(0, false))->set_phony(true));

    def = definition.add("perimeter_extrusion_spacing", coFloatOrPercent, ptFFF);
    def->label = L("Perimeters");
    def->full_label = L("Perimeter spacing");
    def->category = OptionCategory::width;
    def->invalidates_step = posPerimeters;
    def->tooltip = L("Like Perimeter width but spacing is the distance between two perimeter lines (as they overlap a bit, it's not the same)."
        "\nYou can set either 'Spacing', or 'Width'; the other will be calculated, using the perimeter 'Overlap' percentages and default layer height.");
    def->sidetext = L("mm or %");
    def->ratio_over = "nozzle_diameter";
    def->min = 0;
    def->max = 1000;
    def->max_literal = { 10, true };
    def->precision = 6;
    def->can_phony = true;
    def->mode = comAdvancedE | comSuSi;
    def->set_default_value(new ConfigOptionFloatOrPercent(100, true));

    def = definition.add("perimeter_extrusion_change_odd_layers", coFloatOrPercent, ptFFF);
    def->label = L("Perimeters");
    def->full_label = L("Perimeters spacing change on even layers");
    def->category = OptionCategory::width;
    def->invalidates_step = posPerimeters;
    def->tooltip = L("Change width on every even layer (and not on odd layers like the first one) for better overlap with adjacent layers and getting stringer shells. "
                     "Try values about +/- 0.1 with different sign for external and internal perimeters."
                     "\nThis could be combined with extra permeters on even layers."
                     "\nWorks as absolute spacing or a % of the spacing."
                     "\nset 0 to disable");
    def->sidetext = L("mm or %");
    def->mode = comExpert | comSuSi;
    def->set_default_value(new ConfigOptionFloatOrPercent(0, false));

    def = definition.add("perimeter_direction", coEnum, ptFFF);
    def->label = L("Perimeter direction");
    def->category = OptionCategory::perimeter;
    def->invalidates_step = posPerimeters;
    def->tooltip = L("Default direction to print the perimeters (contours and holes): clockwise (CW) or counter-clockwise (CCW).");
    def->set_enum<PerimeterDirection>({
        { "ccw_cw", "Contour: CCW, Holes: CW" },
        { "ccw_ccw","Contour & holes: CCW" },
        { "cw_ccw", "Contour: CW, Holes: CCW" },
        { "cw_cw","Contour & holes: CW" },
    });
    def->mode = comExpert | comSuSi;
    def->set_default_value(new ConfigOptionEnum<PerimeterDirection>(pdCCW_CW));

    def = definition.add("perimeter_fan_speed", coInts, ptFFF);
    def->label = L("Internal Perimeter fan speed");
    def->category = OptionCategory::cooling;
    def->invalidates_step = psGCodeExport;
    def->tooltip = L("This fan speed is enforced during all Perimeter moves"
        "\nSet to 0 to stop the fan."
        "\nIf disabled, default fan speed will be used."
        "\nCan be disabled by disable_fan_first_layers, slowed down by full_fan_speed_layer and increased by low layer time.");
    def->sidetext = L("%");
    def->min = 0;
    def->max = 100;
    def->mode = comExpert | comSuSi;
    def->is_vector_extruder = true;
    def->can_be_disabled = true;
    def->set_default_value(disable_default_option(new ConfigOptionInts({ 100 })));

    def = definition.add("perimeter_loop", coBool, ptFFF);
    def->label = L("Perimeters loop");
    def->full_label = L("Perimeters loop");
    def->category = OptionCategory::perimeter;
    def->invalidates_step = posPerimeters;
    def->tooltip = L("Join the perimeters to create only one continuous extrusion without any z-hop."
        " Long inside travel (from external to holes) are not extruded to give some space to the infill.");
    def->mode = comAdvancedE | comSuSi;
    def->set_default_value(new ConfigOptionBool(false));

    def = definition.add("perimeter_loop_seam", coEnum, ptFFF);
    def->label = L("Seam position");
    def->full_label = L("Perimeter loop seam");
    def->category = OptionCategory::perimeter;
    def->invalidates_step = posPerimeters;
    def->tooltip = L("Position of perimeters starting points.");
    def->set_enum<SeamPosition>({
        { "nearest", "Nearest" },
        { "rear",    "Rear" },
    });
    def->mode = comAdvancedE | comSuSi;
    def->set_default_value(new ConfigOptionEnum<SeamPosition>(spRear));

    def = definition.add("perimeter_overlap", coPercent, ptFFF);
    def->label = L("perimeter overlap");
    def->full_label = L("Perimeter overlap");
    def->category = OptionCategory::width;
    def->invalidates_step = posPerimeters;
    def->tooltip = L("This setting allows you to reduce the overlap between the perimeters, to reduce the impact of the perimeters' artifacts."
        " 100% means that no gap is left, and 0% means that perimeters are not touching each other anymore."
        "\nIt's very experimental, please report about the usefulness.");
    def->sidetext = L("%");
    def->min = 0;
    def->max = 100;
    def->mode = comExpert | comSuSi;
    def->set_default_value(new ConfigOptionPercent(80));

    def = definition.add("perimeter_reverse", coBool, ptFFF);
    def->label = L("Perimeter reversal on even layers");
    def->category = OptionCategory::perimeter;
    def->invalidates_step = posPerimeters;
    def->tooltip = L("On even layers, all perimeter loops are reversed (it disables the overhang reversal, so it doesn't double-reverse)."
                    "That setting will likely create defects on the perimeters, so it's only useful is for materials that have some direction-dependent properties (stress lines).");
    def->mode = comExpert | comSuSi;
    def->set_default_value(new ConfigOptionBool(false));

    def = definition.add("perimeter_round_corners", coBool, ptFFF);
    def->label = L("Round corners");
    def->full_label = L("Round corners for perimeters");
    def->category = OptionCategory::perimeter;
    def->invalidates_step = posPerimeters;
    def->tooltip = L("Internal perimeters will go around sharp corners by turning around instead of making the same sharp corner."
                        " This can help when there are visible holes in sharp corners on internal perimeters."
                        "\nCan incur some more processing time, and corners are a bit less sharp.");
    def->mode = comAdvancedE | comSuSi;
    def->set_default_value(new ConfigOptionBool(false));

    def = definition.add("perimeter_speed", coFloatOrPercent, ptFFF);
    def->label = L("Internal");
    def->full_label = L("Internal perimeters speed");
    def->category = OptionCategory::speed;
    def->invalidates_step = psGCodeExport;
    def->tooltip = L("Speed for perimeters (contours, aka vertical shells)."
        "\nThis can be expressed as a percentage (for example: 80%) over the Default speed."
        "\nSet zero to use autospeed for this feature.");
    def->sidetext = L("mm/s or %");
    def->aliases = { "perimeter_feed_rate" };
    def->ratio_over = "default_speed";
    def->min = 0;
    def->mode = comAdvancedE | comPrusa;
    def->set_default_value(new ConfigOptionFloatOrPercent(60, true));

    def = definition.add("perimeters", coInt, ptFFF);
    def->label = L("Perimeters");
    def->full_label = L("Perimeters count");
    def->category = OptionCategory::perimeter;
    def->invalidates_step = posPerimeters;
    def->tooltip = L("This option sets the number of perimeters to generate for each layer."
                   "\nIf perimeters_hole is activated, then this number is only for contour perimeters."
                   "Note that if a contour perimeter encounter a hole, it will go around like a hole perimeter."
                   "\nNote that Slic3r may increase this number automatically when it detects "
                   "sloping surfaces which benefit from a higher number of perimeters "
                   "if the Extra Perimeters option is enabled.");
    def->sidetext = L("(minimum).");
    def->aliases = { "perimeter_offsets" };
    def->min = 0;
    def->max = 10000;
    def->mode = comSimpleAE | comPrusa;
    def->set_default_value(new ConfigOptionInt(3));

    def = definition.add("perimeters_hole", coInt, ptFFF);
    def->label = L("Max perimeter count for holes");
    def->category = OptionCategory::perimeter;
    def->invalidates_step = posPerimeters;
    def->tooltip = L("This option sets the number of perimeters to have over holes."
                   " Note that if a hole-perimeter fuse with the contour, then it will go around like a contour perimeter."
                   "\nIf disabled, holes will have the same number of perimeters as contour. Cannot be enabled at the same time as Arachne generator."
                   "\nNote that Slic3r may increase this number automatically when it detects "
                   "sloping surfaces which benefit from a higher number of perimeters "
                   "if the Extra Perimeters option is enabled.");
    def->sidetext = L("(minimum).");
    def->min = 0;
    def->max = 10000;
    def->can_be_disabled = true;
    def->mode = comAdvancedE | comSuSi;
    def->set_default_value(disable_default_option(new ConfigOptionInt(0)));

    def = definition.add("post_process", coStrings, ptFFF);
    def->label = L("Post-processing scripts");
    def->category = OptionCategory::customgcode;
    def->invalidates_step = psGCodeExport;
    def->tooltip = L("If you want to process the output G-code through custom scripts, "
                   "just list their absolute paths here."
                   "\nSeparate multiple scripts with a semicolon or a line return.\n!! please use '\\;' here if you want a not-line-separation ';'!!"
                   "\nScripts will be passed the absolute path to the G-code file as the first argument, "
                   "and they can access the Slic3r config settings by reading environment variables."
                   "\nThe script, if passed as a relative path, will also be searched from the slic3r directory, "
                   "the slic3r configuration directory and the user directory.");
    def->multiline = true;
    def->full_width = true;
    def->height = 6;
    def->mode = comExpert | comPrusa;
    def->set_default_value(new ConfigOptionStrings());

    def = definition.add("priming_position", coPoint, ptFFF);
    def->label = L("Priming position");
    def->full_label = L("Priming position");
    def->tooltip = L("Coordinates of the left front corner of the priming patch."
                     "\nIf set to 0,0 then the position is computed automatically.");
    //TODO: enable/disable
    def->category = OptionCategory::customgcode;
    def->invalidates_step = psSkirtBrim;
    def->sidetext = L("mm");
    def->mode = comAdvancedE | comSuSi;
    def->set_default_value(new ConfigOptionPoint(Vec2d(0,0)));

    def = definition.add("printer_custom_variables", coString, ptFFF);
    def->label = L("Custom variables");
    def->full_label = L("Custom Printer variables");
    def->category = OptionCategory::filament;
    def->invalidates_step = psGCodeExport;
    def->tooltip = L("You can add data accessible to custom-gcode macros."
        "\nEach line can define one variable."
        "\nThe format is 'variable_name=value'. the variable name should only have [a-zA-Z0-9] characters or '_'."
        "\nA value that can be parsed as a int or float will be available as a numeric value."
        "\nA value that is enclosed by double-quotes will be available as a string (without the quotes)"
        "\nA value that only takes values as 'true' or 'false' will be a boolean)"
        "\nEvery other value will be parsed as a string as-is."
        "\nAdvice: before using a variable, it's safer to use the function 'default_XXX(variable_name, default_value)'"
        " (enclosed in bracket as it's a script) in case it's not set. You can replace XXX by 'int' 'bool' 'double' 'string'.");
    def->multiline = true;
    def->full_width = true;
    def->height = 13;
    def->mode = comAdvancedE | comSuSi;
    def->set_default_value(new ConfigOptionString{ "" });

    def = definition.add("fan_printer_min_speed", coInt, ptFFF);
    def->label = L("Minimum fan speed");
    def->full_label = L("Minimum fan speed");
    def->category = OptionCategory::general;
    def->invalidates_step = psGCodeExport;
    def->tooltip = L("This setting represents the minimum fan speed (like minimum PWM) your fan needs to work.");
    def->sidetext = L("%");
    def->min = 0;
    def->max = 100;
    def->mode = comAdvancedE | comSuSi;
    def->set_default_value(new ConfigOptionInt(0));

    def = definition.add("print_bed_temperature", coInt, ptFFF);
    def->label = L("Bed Temperature");
    def->category = OptionCategory::filament;
    def->invalidates_step = psGCodeExport;
    def->tooltip = L("Override the temperature of the bed."
                    "\nIf disabled, uses the highest bed temperature from all filaments used in the first layer.");
    def->sidetext = L("°C");
    def->min = 0;
    def->mode = comExpert | comSuSi;
    def->can_be_disabled = true;
    def->set_default_value(disable_default_option(new ConfigOptionInt(60)));

    def = definition.add("print_first_layer_bed_temperature", coInt, ptFFF);
    def->label = L("First Layer Bed Temperature");
    def->category = OptionCategory::filament;
    def->invalidates_step = psGCodeExport;
    def->tooltip = L("Override the temperature of the bed while printing the first layer."
                    "\nIf disabled, uses the highest first layer bed temperature from all filaments used in the first layer."
                    "\nSet to 0 to prevent the slicer to do any first layer bed command.");
    def->sidetext = L("°C");
    def->min = 0;
    def->mode = comExpert | comSuSi;
    def->can_be_disabled = true;
    def->set_default_value(disable_default_option(new ConfigOptionInt(60)));

    def = definition.add("print_first_layer_temperature", coInt, ptFFF);
    def->label = L("First Layer Temperature");
    def->category = OptionCategory::filament;
    def->invalidates_step = psGCodeExport;
    def->tooltip = L("Override the temperature of the extruder (for the first layer). Avoid making too many changes, it won't stop for cooling/heating."
        "May only work on Height range modifiers.");
    def->sidetext = L("°C");
    def->min = 0;
    def->mode = comExpert | comSuSi;
    def->can_be_disabled = true;
    def->set_default_value(disable_default_option(new ConfigOptionInt(200)));

    def = definition.add("print_retract_length", coFloat, ptFFF);
    def->label = L("Retraction length");
    def->category = OptionCategory::filament;
    def->invalidates_step = psGCodeExport;
    def->tooltip = L("Override the retract_length setting from the printer config. Used for calibration.");
    def->min = 0;
    def->mode = comExpert | comSuSi;
    def->can_be_disabled = true;
    def->set_default_value(disable_default_option(new ConfigOptionFloat(0)));

    def = definition.add("print_retract_lift", coFloat, ptFFF);
    def->label = L("Z-lift override");
    def->category = OptionCategory::filament;
    def->invalidates_step = psGCodeExport;
    def->tooltip = L("Set the new lift-z value for this override. 0 will disable the z-lift. -& to disable. May only work on Height range modifiers.");
    def->sidetext = L("mm");
    def->min = 0;
    def->mode = comExpert | comSuSi;
    def->can_be_disabled = true;
    def->set_default_value(disable_default_option(new ConfigOptionFloat(0)));

    def = definition.add("print_temperature", coInt, ptFFF);
    def->label = L("Temperature");
    def->category = OptionCategory::filament;
    def->invalidates_step = psGCodeExport;
    def->tooltip = L("Override the temperature of the extruder. Avoid making too many changes, it won't stop for cooling/heating."
        " May only work on Height range modifiers.");
    def->sidetext = L("°C");
    def->min = 0;
    def->mode = comExpert | comSuSi;
    def->can_be_disabled = true;
    def->set_default_value(disable_default_option(new ConfigOptionInt(200)));

    def = definition.add("printer_model", coString, ptFFF);
    def->label = L("Printer type");
    def->tooltip = L("Type of the printer.");
    def->mode = comNone | comPrusa; // note: hidden setting
    def->set_default_value(new ConfigOptionString());
    def->cli = ConfigOptionDef::nocli;

    def = definition.add("printer_notes", coString, ptFFF);
    def->label = L("Printer notes");
    def->category = OptionCategory::notes;
    def->invalidates_step = psGCodeExport;
    def->tooltip = L("You can put your notes regarding the printer here.");
    def->multiline = true;
    def->full_width = true;
    def->height = 13;
    def->mode = comAdvancedE | comPrusa;
    def->set_default_value(new ConfigOptionString(""));

    def = definition.add("printer_vendor", coString, ptFFF);
    def->label = L("Printer vendor");
    def->tooltip = L("Name of the printer vendor.");
    def->mode = comNone | comPrusa; // note: hidden setting
    def->set_default_value(new ConfigOptionString());
    def->cli = ConfigOptionDef::nocli;

    def = definition.add("printer_variant", coString, ptFFF);
    def->label = L("Printer variant");
    def->tooltip = L("Name of the printer variant. For example, the printer variants may be differentiated by a nozzle diameter.");
    def->mode = comNone | comPrusa; // note: hidden setting
    def->set_default_value(new ConfigOptionString());
    def->cli = ConfigOptionDef::nocli;

    def = definition.add("print_settings_id", coString, ptFFF);
    def->mode = comNone | comPrusa; // note: hidden setting
    def->set_default_value(new ConfigOptionString(""));
    def->cli = ConfigOptionDef::nocli;

    def = definition.add("print_settings_modified", coBool, ptFFF);
    def->mode = comNone | comPrusa; // note: hidden setting
    def->set_default_value(new ConfigOptionBool(false));
    def->cli = ConfigOptionDef::nocli;

    def = definition.add("printer_settings_id", coString, ptFFF);
    def->mode = comNone | comPrusa; // note: hidden setting
    def->set_default_value(new ConfigOptionString(""));
    def->cli = ConfigOptionDef::nocli;

    def = definition.add("printer_settings_modified", coBool, ptFFF);
    def->mode = comNone | comPrusa; // note: hidden setting
    def->set_default_value(new ConfigOptionBool(false));
    def->cli = ConfigOptionDef::nocli;

    def = definition.add("physical_printer_settings_id", coString, ptFFF);
    def->mode = comNone | comPrusa; // note: hidden setting
    def->set_default_value(new ConfigOptionString(""));
    def->cli = ConfigOptionDef::nocli;

    def = definition.add("raft_contact_distance", coFloat, ptFFF);
    def->label = L("Raft contact Z distance");
    def->category = OptionCategory::support;
    def->invalidates_step = posSlice;
    def->tooltip = L("The vertical distance between object and raft. Ignored for soluble interface. It uses the same type as the support z-offset type.");
    def->sidetext = L("mm");
    def->min = 0;
    def->mode = comAdvancedE | comPrusa;
    def->set_default_value(new ConfigOptionFloat(0.1));

    def = definition.add("raft_contact_distance_type", coEnum, ptFFF);
    def->label = L("Type");
    def->full_label = L("Raft contact distance type");
    def->category = OptionCategory::support;
    def->invalidates_step = posSlice;
    def->tooltip = L("How to compute the vertical z-distance.\n"
        "From filament: it uses the nearest bit of the filament. When a bridge is extruded, it goes below the current plane.\n"
        "From plane: it uses the plane-z. Same as 'from filament' if no 'bridge' is extruded.\n"
        "None: No z-offset. Useful for Soluble supports.\n");
    def->set_enum<SupportZDistanceType>({
        { "filament", L("From filament") },
        { "plane",    L("From plane") },
        { "none",     L("None (soluble)") }
    });
    def->mode = comAdvancedE | comSuSi;
    def->set_default_value(new ConfigOptionEnum<SupportZDistanceType>(zdPlane));

    def = definition.add("raft_expansion", coFloat, ptFFF);
    def->label = L("Raft expansion");
    def->category = OptionCategory::support;
    def->invalidates_step = posSupportMaterial;
    def->tooltip = L("Expansion of the raft in XY plane for better stability.");
    def->sidetext = L("mm");
    def->min = 0;
    def->mode = comExpert | comPrusa;
    def->set_default_value(new ConfigOptionFloat(1.5));

    def = definition.add("raft_first_layer_density", coPercent, ptFFF);
    def->label = L("First layer density");
    def->full_label = L("Raft first layer density");
    def->category = OptionCategory::support;
    def->invalidates_step = posSupportMaterial;
    def->tooltip = L("Density of the first raft or support layer.");
    def->sidetext = L("%");
    def->min = 10;
    def->max = 100;
    def->mode = comExpert | comPrusa;
    def->set_default_value(new ConfigOptionPercent(90));

    def = definition.add("raft_first_layer_expansion", coFloat, ptFFF);
    def->label = L("First layer expansion");
    def->full_label = L("Raft first layer expansion");
    def->category = OptionCategory::support;
    def->invalidates_step = posSupportMaterial;
    def->tooltip = L("Expansion of the first raft or support layer to improve adhesion to print bed.");
    def->sidetext = L("mm");
    def->min = 0;
    def->mode = comExpert | comPrusa;
    def->set_default_value(new ConfigOptionFloat(3.));

    def = definition.add("raft_layers", coInt, ptFFF);
    def->label = L("Raft layers");
    def->category = OptionCategory::support;
    def->invalidates_step = posSlice;
    def->tooltip = L("The object will be raised by this number of layers, and support material "
        "will be generated under it.");
    def->sidetext = L("layers");
    def->min = 0;
    def->mode = comAdvancedE | comPrusa;
    def->set_default_value(new ConfigOptionInt(0));

    def = definition.add("raft_layer_height", coFloatOrPercent, ptFFF);
    def->label = L("Raft layer height");
    def->category = OptionCategory::support;
    def->invalidates_step = posSlice;
    def->tooltip = L("Maximum layer height for the raft, after the first layer that uses the first layer height, and before the interface layers."
        "\nCan be a % of the nozzle diameter"
        "\nIf set to 0, the support layer height will be used.");
    def->sidetext = L("mm");
    def->ratio_over = "nozzle_diameter";
    def->min = 0;
    def->mode = comAdvancedE | comSuSi;
    def->set_default_value(new ConfigOptionFloatOrPercent(0, false));

    def = definition.add("raft_interface_layer_height", coFloatOrPercent, ptFFF);
    def->label = L("Raft interface layer height");
    def->category = OptionCategory::support;
    def->invalidates_step = posSlice;
    def->tooltip = L("Maximum layer height for the raft interface."
        "\nCan be a % of the nozzle diameter"
        "\nIf set to 0, the support layer height will be used.");
    def->sidetext = L("mm");
    def->ratio_over = "nozzle_diameter";
    def->min = 0;
    def->mode = comAdvancedE | comSuSi;
    def->set_default_value(new ConfigOptionFloatOrPercent(0, false));

    def = definition.add("region_gcode", coString, ptFFF);
    def->label = L("Per region G-code");
    def->category = OptionCategory::output;
    def->invalidates_step = psGCodeExport;
    def->tooltip = L("This code is inserted when a region is starting to print something (infill, perimeter, ironing)."
                     " It's main advantage is when you use it as a object modifier(right click on a model to add it there)"
                     "\nSpecial variables: 'layer_num','layer_z'");
    def->multiline = true;
    def->full_width = true;
    def->height = 10;
    def->mode = comExpert | comSuSi;
    def->set_default_value(new ConfigOptionString(""));

    def = definition.add("resolution", coFloat, ptFFF);
    def->label = L("Slice resolution");
    def->category = OptionCategory::slicing;
    def->invalidates_step = posSlice;
    def->tooltip = L("Minimum detail resolution, used to simplify the input file for speeding up "
        "the slicing job and reducing memory usage. High-resolution models often carry "
        "more details than printers can render. Set zero to disable any simplification "
        "and use full resolution from input. "
        "\nNote: Slic3r has an internal working resolution of 0.0001mm."
        "\nInfill & Thin areas are simplified up to 0.0125mm.");
    def->sidetext = L("mm");
    def->min = 0;
    def->precision = 8;
    def->mode = comExpert | comPrusa;
    def->set_default_value(new ConfigOptionFloat(0.0125));

    //note: replaced by gcode_min_resolution in printer's profile
    //def = definition.add("gcode_resolution", coFloat);
    //def->label = L("G-code resolution");
    //def->tooltip = L("Maximum deviation of exported G-code paths from their full resolution counterparts. "
    //    "Very high resolution G-code requires huge amount of RAM to slice and preview, "
    //    "also a 3D printer may stutter not being able to process a high resolution G-code in a timely manner. "
    //    "On the other hand, a low resolution G-code will produce a low poly effect and because "
    //    "the G-code reduction is performed at each layer independently, visible artifacts may be produced.");
    //def->sidetext = L("mm");
    //def->min = 0;
    //def->mode = comExpert | comPrusa;
    //def->set_default_value(new ConfigOptionFloat(0.0));

    def = definition.add("gcode_command_buffer", coInt, ptFFF);
    def->label = L("Command buffer");
    def->category = OptionCategory::speed;
    def->invalidates_step = psGCodeExport;
    def->tooltip = L("Buffer the firmware has for gcode commands. Allow to have some burst of command with a rate over 'max_gcode_per_second' for a few instant.");
    def->min = 0;
    def->mode = comExpert | comSuSi;
    def->set_default_value(new ConfigOptionInt(10));

    def = definition.add("gcode_min_length", coFloatOrPercent, ptFFF);
    def->label = L("Minimum extrusion length");
    def->category = OptionCategory::speed;
    def->invalidates_step = psGCodeExport;
    def->tooltip = L("When outputting gcode, this setting ensure that there is almost no commands more than this value apart."
        " Be sure to also use max_gcode_per_second instead, as it's much better when you have very different speeds for features"
        " (Too many too small commands may overload the firmware / connection)."
        "\nDisabled if set to 0.");
    def->sidetext = L("mm or %");
    def->min = 0;
    def->precision = 6;
    def->mode = comExpert | comSuSi;
    def->can_be_disabled = true;
    def->set_default_value(enable_default_option(new ConfigOptionFloatOrPercent(0.02, false)));
    def->aliases = {"min_length"};

    def = definition.add("gcode_min_resolution", coFloatOrPercent, ptFFF);
    def->label = L("minimum resolution");
    def->category = OptionCategory::speed;
    def->invalidates_step = posSlice;
    def->tooltip = L("Maximum deviation of exported G-code paths from their full resolution counterparts"
        " when some commands are culled by 'gcode_min_length' or 'max_gcode_per_second' to not overload the firmware."
        "\nCan be a % of perimeter width.");
    def->sidetext = L("mm or %");
    def->min = 0;
    def->precision = 6;
    def->mode = comExpert | comSuSi;
    def->set_default_value(new ConfigOptionFloatOrPercent(0, false));

    def = definition.add("resolution_internal", coFloat, ptFFF);
    def->label = L("Internal resolution");
    def->category = OptionCategory::slicing;
    def->invalidates_step = posSlice;
    def->tooltip = L("Minimum detail resolution, used for internal structures (gapfill and some infill patterns)."
            "\nDon't put a too-small value, as it may create too many very small segments that may be difficult to display and print if your main resolution parameter is also very small.");
    def->sidetext = L("mm");
    def->min = 0.0001;
    def->precision = 8;
    def->mode = comExpert | comSuSi;
    def->set_default_value(new ConfigOptionFloat(0.025));

    def = definition.add("retract_before_travel", coFloats, ptFFF);
    def->label = L("Minimum travel after retraction");
    def->category = OptionCategory::extruders;
    def->invalidates_step = psGCodeExport;
    def->tooltip = L("Retraction is not triggered when travel moves are shorter than this length.");
    def->sidetext = L("mm");
    def->mode = comAdvancedE | comPrusa;
    def->min = 0;
    def->is_vector_extruder = true;
    def->set_default_value(new ConfigOptionFloats { 2. });

    def = definition.add("retract_lift_before_travel", coFloats, ptFFF);
    def->label = L("Minimum travel after z lift");
    def->category = OptionCategory::extruders;
    def->invalidates_step = psGCodeExport;
    def->tooltip = L("Z lift is not triggered when travel moves are shorter than this length.");
    def->sidetext = L("mm");
    def->mode = comAdvancedE | comSuSi;
    def->min = 0;
    def->is_vector_extruder = true;
    def->set_default_value(new ConfigOptionFloats { 2. });

    def = definition.add("retract_before_wipe", coPercents, ptFFF);
    def->label = L("Retract amount before wipe");
    def->category = OptionCategory::extruders;
    def->invalidates_step = psGCodeExport;
    def->tooltip = L("With bowden extruders, it may be wise to do some amount of quick retract "
                   "before doing the wipe movement.");
    def->sidetext = L("%");
    def->mode = comAdvancedE | comPrusa;
    def->is_vector_extruder = true;
    def->set_default_value(new ConfigOptionPercents { 0. });

    def = definition.add("retract_layer_change", coBools, ptFFF);
    def->label = L("Retract on layer change");
    def->category = OptionCategory::extruders;
    def->invalidates_step = psGCodeExport;
    def->tooltip = L("This flag enforces a retraction whenever a Z move is done (before it).");
    def->mode = comAdvancedE | comPrusa;
    def->is_vector_extruder = true;
    def->set_default_value(new ConfigOptionBools { false });

    def = definition.add("retract_length", coFloats, ptFFF);
    def->label = L("Retraction length");
    def->full_label = L("Retraction Length");
    def->category = OptionCategory::extruders;
    def->invalidates_step = psGCodeExport;
    def->tooltip = L("When retraction is triggered, filament is pulled back by the specified amount "
                   "(the length is measured on raw filament, before it enters the extruder).");
    def->sidetext = L("mm (zero to disable)");
    def->min = 0;
    def->mode = comSimpleAE | comPrusa;
    def->is_vector_extruder = true;
    def->set_default_value(new ConfigOptionFloats { 2. });

    def = definition.add("travel_slope", coFloats, ptFFF);
    def->label = L("Ramping slope angle");
    def->invalidates_step = psGCodeExport;
    def->tooltip = L("Minimum slope of the ramp in the initial phase of the travel."
                    " If the travel isn't long enough, the angle will be increased."
                    "\n90° means a direct lift, like if there was no ramp."
                    "\n0° means that the lift will always be hit at the end of the travel.");
    def->sidetext = L("°");
    def->min = 0;
    def->max = 90;
    def->mode = comAdvancedE | comPrusa;
    def->is_vector_extruder = true;
    def->set_default_value(new ConfigOptionFloats{0.0});

    def = definition.add("travel_ramping_lift", coBools, ptFFF);
    def->label = L("Use ramping lift");
    def->invalidates_step = psGCodeExport;
    def->tooltip = L("Generates a ramping lift instead of lifting the extruder directly upwards. "
                     "The travel is split into two phases: the ramp and the standard horizontal travel. "
                     "This option helps reduce stringing."
                     "\nAlso works for the z move when a layer change occurs.");
    def->mode = comAdvancedE | comPrusa;
    def->is_vector_extruder = true;
    def->set_default_value(new ConfigOptionBools{ false });

    // why not reuse retract_lift ? because it's a max? My current impl enforced the lift, so it's okay for me to remove it.
    // def = definition.add("travel_max_lift", coFloats);
    // def->label = L("Maximum ramping lift");
    // def->tooltip = L("Maximum lift height of the ramping lift. It may not be reached if the next position "
                     // "is close to the old one.");
    // def->sidetext = L("mm");
    // def->min = 0;
    // def->max_literal = {1000, false};
    // def->mode = comAdvancedE | comPrusa;
    // def->is_vector_extruder = true;
    // def->set_default_value(new ConfigOptionFloats{0.0});

    def = definition.add("travel_lift_before_obstacle", coBools, ptFFF);
    def->label = L("Steeper ramp before obstacles");
    def->invalidates_step = psGCodeExport;
    def->tooltip = L("If enabled, PrusaSlicer detects obstacles along the travel path and makes the slope steeper "
                     "in case an obstacle might be hit during the initial phase of the travel.");
    def->mode = comExpert | comPrusa;
    def->is_vector_extruder = true;
    def->set_default_value(new ConfigOptionBools{false});

    def = definition.add("retract_length_toolchange", coFloats, ptFFF);
    def->label = L("Length");
    def->full_label = L("Retraction Length (Toolchange)");
    def->invalidates_step = psGCodeExport;
    def->tooltip = L("When retraction is triggered before changing tool, filament is pulled back "
                   "by the specified amount (the length is measured on raw filament, before it enters "
                   "the extruder)."
                    "\nNote: This value will be unretracted when this extruder will load the next time.");
    def->sidetext = L("mm (zero to disable)");
    def->mode = comExpert | comPrusa;
    def->min = 0;
    def->is_vector_extruder = true;
    def->set_default_value(new ConfigOptionFloats { 10. });

    def = definition.add("retract_lift", coFloats, ptFFF);
    def->label = L("Lift height");
    def->category = OptionCategory::extruders;
    def->invalidates_step = psGCodeExport;
    def->tooltip = L("If you set this to a positive value, the extruder is quickly raised every time a retraction "
                   "is triggered. When using multiple extruders, only the setting for the first extruder "
                   "will be considered.");
    def->sidetext = L("mm");
    def->min = 0;
    def->max_literal = {1000, false};
    def->mode = comSimpleAE | comPrusa;
    def->is_vector_extruder = true;
    def->set_default_value(new ConfigOptionFloats{0.});

    def = definition.add("retract_lift_above", coFloats, ptFFF);
    def->label = L("Above Z");
    def->full_label = L("Only lift Z above");
    def->category = OptionCategory::extruders;
    def->invalidates_step = psGCodeExport;
    def->tooltip = L("If you set this to a positive value, Z lift will only take place above the specified "
                   "absolute Z. You can tune this setting for skipping lift on the first layers.");
    def->sidetext = L("mm");
    def->mode = comAdvancedE | comPrusa;
    def->is_vector_extruder = true;
    def->set_default_value(new ConfigOptionFloats { 0. });

    def = definition.add("retract_lift_below", coFloats, ptFFF);
    def->label = L("Below Z");
    def->full_label = L("Only lift Z below");
    def->category = OptionCategory::extruders;
    def->invalidates_step = psGCodeExport;
    def->tooltip = L("If you set this to a positive value, Z lift will only take place below "
                   "the specified absolute Z. You can tune this setting for limiting lift "
                   "to the first layers.");
    def->sidetext = L("mm");
    def->mode = comAdvancedE | comPrusa;
    def->is_vector_extruder = true;
    def->set_default_value(new ConfigOptionFloats { 0. });

    def = definition.add("retract_lift_first_layer", coBools, ptFFF);
    def->label = L("Enforce on first layer");
    def->full_label = L("Enforce lift on first layer");
    def->category = OptionCategory::extruders;
    def->invalidates_step = psGCodeExport;
    def->tooltip = L("Select this option to enforce z-lift on the first layer."
        "\nUseful to still use the lift on the first layer even if the 'Only lift Z below' (retract_lift_above) is higher than 0.");
    def->mode = comAdvancedE | comSuSi;
    def->is_vector_extruder = true;
    def->set_default_value(new ConfigOptionBools{ false });

    def = definition.add("retract_lift_top", coStrings, ptFFF);
    def->label = L("On surfaces");
    def->full_label = L("Lift only on");
    def->category = OptionCategory::extruders;
    def->invalidates_step = psGCodeExport;
    def->tooltip = L("Select this option to not use/enforce the z-lift on a top surface.");
    def->gui_type = ConfigOptionDef::GUIType::f_enum_open;
    def->gui_flags = "show_value";
    def->set_enum_values(ConfigOptionDef::GUIType::select_open,
        { "All surfaces", "Not on top", "Only on top"});
    def->mode = comAdvancedE | comSuSi;
    def->is_vector_extruder = true;
    def->set_default_value(new ConfigOptionStrings{ "All surfaces" });


    def = definition.add("retract_restart_extra", coFloats, ptFFF);
    def->label = L("Deretraction extra length");
    def->invalidates_step = psGCodeExport;
    def->tooltip = L("When the retraction is compensated after the travel move, the extruder will push "
                   "this additional amount of filament. This setting is rarely needed.");
    def->sidetext = L("mm");
    def->mode = comExpert | comPrusa;
    def->is_vector_extruder = true;
    def->set_default_value(new ConfigOptionFloats { 0. });

    def = definition.add("retract_restart_extra_toolchange", coFloats, ptFFF);
    def->label = L("Extra length on restart");
    def->full_label = L("Extra length on toolchange restart");
    def->invalidates_step = psGCodeExport;
    def->tooltip = L("When the retraction is compensated after changing tool, the extruder will push "
                    "this additional amount of filament"
                    " (but not on the first extruder after start, as it should already be loaded).");
    def->sidetext = L("mm");
    def->mode = comExpert | comPrusa;
    def->is_vector_extruder = true;
    def->set_default_value(new ConfigOptionFloats { 0. });

    def = definition.add("retract_restart_wipe_toolchange", coPercents, ptFFF);
    def->label = L("Wipe the unretraction");
    def->full_label = L("Wipe the unretraction (Toolchange)");
    def->invalidates_step = psGCodeExport;
    def->tooltip = L("When unretraction is triggered after changing tool in a wipe tower, the last part of the "
                     "unretraction is made into a wipe move instead of a static unretraction on top of the wipe "
                     "tower. This percentage is the perdcetage of the unretraction that is made into a wipe move");
    def->sidetext = L("%");
    def->mode = comExpert | comSuSi;
    def->is_vector_extruder = true;
    def->set_default_value(new ConfigOptionPercents { 20. });

    def = definition.add("retract_restart_toolchange_on_perimeter", coBools, ptFFF);
    def->label = L("Unretract on perimeter");
    def->full_label = L("Wipe the unretraction (Toolchange)");
    def->invalidates_step = psGCodeExport;
    def->tooltip = L("When unretraction is triggered after changing tool in a wipe tower, the first bit is done on "
                     "the perimeter instead than in the air next to it. This may prevent too much oozing while unretracting.");
    def->mode = comExpert | comSuSi;
    def->is_vector_extruder = true;
    def->set_default_value(new ConfigOptionBools { true });

    def = definition.add("retract_speed", coFloats, ptFFF);
    def->label = L("Retraction Speed");
    def->full_label = L("Retraction Speed");
    def->category = OptionCategory::extruders;
    def->invalidates_step = psGCodeExport;
    def->tooltip = L("The speed for retractions (this only applies to the extruder motor).");
    def->sidetext = L("mm/s");
    def->mode = comAdvancedE | comPrusa;
    def->min = 0.001;
    def->is_vector_extruder = true;
    def->set_default_value(new ConfigOptionFloats { 40. });

    def = definition.add("deretract_speed", coFloats, ptFFF);
    def->label = L("Deretraction Speed");
    def->full_label = L("Deretraction Speed");
    def->category = OptionCategory::extruders;
    def->invalidates_step = psGCodeExport;
    def->tooltip = L("The speed for loading of a filament into extruder after retraction "
                   "(this only applies to the extruder motor). If left as zero, the retraction speed is used.");
    def->sidetext = L("mm/s");
    def->mode = comAdvancedE | comPrusa;
    def->is_vector_extruder = true;
    def->set_default_value(new ConfigOptionFloats { 0. });

    def = definition.add("seam_position", coEnum, ptFFF);
    def->label = L("Seam position");
    def->category = OptionCategory::perimeter;
    def->invalidates_step = psGCodeExport;
    def->tooltip = L("Position of perimeters' starting points."
                    "\nCost-based option let you choose the angle and travel cost. A high angle cost will place the seam where it can be hidden by a corner"
                    ", the travel cost place the seam near the last position (often at the end of the previous infill). Default is 60 % and 100 %."
                    " There is also the visibility and the overhang cost, but they are static."
                    "\n Scattered: seam is placed at a random position on external perimeters"
                    "\n Random: seam is placed at a random position for all perimeters"
                    "\n Aligned: seams are grouped in the best place possible (minimum 6 layers per group)"
                    "\n Contiguous: seam is placed over a seam from the previous layer (useful with enforcers)"
                    "\n Rear: seam is placed at the far side (highest Y coordinates)");
    def->set_enum<SeamPosition>({
        { "cost",       L("Cost-based") },
        { "random",     L("Scattered") },
        { "allrandom",  L("Random") },
        { "aligned",    L("Aligned") },
        { "contiguous", L("Contiguous") },

        { "rear",       L("Rear") }
    });
    def->mode = comSimpleAE | comPrusa | comSuSi;
    def->set_default_value(new ConfigOptionEnum<SeamPosition>(spCost));

    def = definition.add("seam_angle_cost", coPercent, ptFFF);
    def->label = L("Angle cost");
    def->full_label = L("Seam angle cost");
    def->category = OptionCategory::perimeter;
    def->invalidates_step = psGCodeExport;
    def->tooltip = L("Cost of placing the seam at a bad angle. The worst angle (max penalty) is when it's flat."
        "\n100% is the default penalty");
    def->sidetext = L("%");
    def->min = 0;
    def->max = 1000;
    def->mode = comExpert | comSuSi;
    def->set_default_value(new ConfigOptionPercent(60));

    def = definition.add("seam_gap", coFloatsOrPercents, ptFFF);
    def->label = L("Seam gap");
    def->category = OptionCategory::extruders;
    def->invalidates_step = posInfill;
    def->tooltip = L("To avoid visible seam, the extrusion can be stoppped a bit before the end of the loop."
        "\nCan be a mm or a % of the current extruder diameter.");
    def->sidetext = L("mm or %");
    def->min = 0;
    def->max_literal = { 5, false };
    def->mode = comExpert | comSuSi;
    def->is_vector_extruder = true;
    def->set_default_value(new ConfigOptionFloatsOrPercents{ FloatOrPercent{15,true} });

    def = definition.add("seam_gap_external", coFloatsOrPercents, ptFFF);
    def->label = L("Seam gap for external perimeters");
    def->category = OptionCategory::extruders;
    def->invalidates_step = posInfill;
    def->tooltip = L("To avoid visible seam, the extrusion can be stoppped a bit before the end of the loop."
        "\n this setting is enforced only for external perimeter. It overrides 'seam_gap' if different than 0"
        "\nCan be a mm or a % of the current seam gap.");
    def->sidetext = L("mm or %");
    def->min = 0;
    def->max_literal = { 5, false };
    def->mode = comExpert | comSuSi;
    def->is_vector_extruder = true;
    def->set_default_value(new ConfigOptionFloatsOrPercents{ FloatOrPercent{0,false} });

    def = definition.add("seam_notch_all", coFloatOrPercent, ptFFF);
    def->label = L("for everything");
    def->full_label = L("Seam notch");
    def->category = OptionCategory::perimeter;
    def->invalidates_step = psGCodeExport;
    def->tooltip = L("It's sometimes very problematic to have a little bulge from the seam."
        " This setting move the seam inside the part, in a little cavity (for every seams in external perimeters, unless it's in an overhang)."
        "\nThe size of the cavity is in mm or a % of the external perimeter width. It's overridden by the two other 'seam notch' setting when applicable."
        "\nSet zero to disable.");
    def->sidetext = L("mm or %");
    def->min = 0;
    def->max_literal = { 5, false };
    def->mode = comExpert | comSuSi;
    def->set_default_value(new ConfigOptionFloatOrPercent(0, false));

    def = definition.add("seam_notch_angle", coFloat, ptFFF);
    def->label = L("max angle");
    def->full_label = L("Seam notch maximum angle");
    def->category = OptionCategory::perimeter;
    def->invalidates_step = psGCodeExport;
    def->tooltip = L("If the (external) angle at the seam is higher than this value, then no notch will be set. If the angle is too high, there isn't enough room for the notch."
                    "\nCan't be lower than 180° or it filters everything. At 360, it allows everything.");
    def->sidetext = L("°");
    def->min = 180;
    def->max = 360;
    def->mode = comExpert | comSuSi;
    def->set_default_value(new ConfigOptionFloat(250));

    def = definition.add("seam_notch_inner", coFloatOrPercent, ptFFF);
    def->label = L("for round holes");
    def->full_label = L("Seam notch for round holes");
    def->category = OptionCategory::perimeter;
    def->invalidates_step = psGCodeExport;
    def->tooltip = L("In convex holes (circular/oval), it's sometimes very problematic to have a little bulge from the seam."
        " This setting move the seam inside the part, in a little cavity (for all external perimeters in convex holes, unless it's in an overhang)."
        "\nThe size of the cavity is in mm or a % of the external perimeter width"
        "\nSet zero to disable.");
    def->sidetext = L("mm or %");
    def->min = 0;
    def->max = 50;
    def->max_literal = { 5, false };
    def->mode = comExpert | comSuSi;
    def->set_default_value(new ConfigOptionFloatOrPercent(0, false));

    def = definition.add("seam_notch_outer", coFloatOrPercent, ptFFF);
    def->label = L("for round perimeters");
    def->full_label = L("Seam notch for round perimeters");
    def->category = OptionCategory::perimeter;
    def->invalidates_step = psGCodeExport;
    def->tooltip = L("In convex perimeters (circular/oval), it's sometimes very problematic to have a little bulge from the seam."
        " This setting move the seam inside the part, in a little cavity (for all external perimeters if the path is convex, unless it's in an overhang)."
        "\nThe size of the cavity is in mm or a % of the external perimeter width"
        "\nSet zero to disable.");
    def->sidetext = L("mm or %");
    def->min = 0;
    def->max = 50;
    def->max_literal = { 5, false };
    def->mode = comExpert | comSuSi;
    def->set_default_value(new ConfigOptionFloatOrPercent(0, false));

    def = definition.add("seam_travel_cost", coPercent, ptFFF);
    def->label = L("Travel cost");
    def->full_label = L("Seam travel cost");
    def->category = OptionCategory::perimeter;
    def->invalidates_step = psGCodeExport;
    def->tooltip = L("Cost of moving the extruder. The highest penalty is when the point is the furthest from the position of the extruder before extruding the external perimeter");
    def->sidetext = L("%");
    def->min = 0;
    def->max = 1000;
    def->mode = comExpert | comSuSi;
    def->set_default_value(new ConfigOptionPercent(100));

    def = definition.add("seam_visibility", coBool, ptFFF);
    def->label = L("use visibility check");
    def->full_label = L("Seam visibility check");
    def->category = OptionCategory::perimeter;
    def->invalidates_step = psGCodeExport;
    def->tooltip = L("Check and penalize seams that are the most visible. launch rays to check from how many direction a point is visible."
        "\nThis is a compute-intensive option.");
    def->mode = comExpert | comSuSi;
    def->set_default_value(new ConfigOptionBool(true));

    def = definition.add("staggered_inner_seams", coBool, ptFFF);
    def->label = L("Staggered inner seams");
    // TRN PrintSettings: "Staggered inner seams"
    def->category = OptionCategory::perimeter;
    def->tooltip = L("This option causes the inner seams to be shifted backwards based on their depth, forming a zigzag pattern.");
    def->mode = comAdvancedE | comPrusa;
    def->set_default_value(new ConfigOptionBool(false));

#if 0
    def = definition.add("seam_preferred_direction", coFloat, ptFFF);
//    def->gui_type = ConfigOptionDef::GUIType::slider;
    def->label = L("Direction");
    def->sidetext = L("°");
    def->full_label = L("Preferred direction of the seam");
    def->tooltip = L("Seam preferred direction");
    def->min = 0;
    def->max = 360;
    def->mode = comSimpleAE | comPrusa;
    def->set_default_value(new ConfigOptionFloat(0));

    def = definition.add("seam_preferred_direction_jitter", coFloat, ptFFF);
//    def->gui_type = ConfigOptionDef::GUIType::slider;
    def->label = L("Jitter");
    def->sidetext = L("°");
    def->full_label = L("Seam preferred direction jitter");
    def->tooltip = L("Preferred direction of the seam - jitter");
    def->min = 0;
    def->max = 360;
    def->mode = comSimpleAE | comPrusa;
    def->set_default_value(new ConfigOptionFloat(30));
#endif
    def = definition.add("skirt_brim", coInt, ptFFF);
    def->label = L("Brim");
    def->full_label = L("Skirt brim");
    def->category = OptionCategory::skirtBrim;
    def->invalidates_step = psSkirtBrim;
    def->tooltip = L("Extra skirt lines on the first layer.");
    def->sidetext = L("lines");
    def->mode = comExpert | comSuSi;
    def->set_default_value(new ConfigOptionInt(0));

    def = definition.add("skirt_distance", coFloat, ptFFF);
    def->label = L("Distance from object");
    def->category = OptionCategory::skirtBrim;
    def->invalidates_step = psSkirtBrim;
    def->tooltip = L("Distance between skirt and object(s) ; or from the brim if using draft shield or you set 'skirt_distance_from_brim'.");
    def->sidetext = L("mm");
    def->min = 0;
    def->mode = comAdvancedE | comPrusa;
    def->set_default_value(new ConfigOptionFloat(6));

    def = definition.add("skirt_distance_from_brim", coBool, ptFFF);
    def->label = L("from brim");
    def->full_label = L("Skirt distance from brim");
    def->category = OptionCategory::skirtBrim;
    def->invalidates_step = psSkirtBrim;
    def->tooltip = L("The distance is computed from the brim and not from the objects");
    def->mode = comAdvancedE | comSuSi;
    def->set_default_value(new ConfigOptionBool(true));

    def = definition.add("skirt_height", coInt, ptFFF);
    def->label = L("Skirt height");
    def->category = OptionCategory::skirtBrim;
    def->invalidates_step = psSkirtBrim;
    def->tooltip = L("Height of skirt expressed in layers. Set this to a tall value to use skirt "
        "as a shield against drafts.");
    def->sidetext = L("layers");
    def->mode = comAdvancedE | comPrusa;
    def->set_default_value(new ConfigOptionInt(1));

    def = definition.add("skirt_extrusion_width", coFloatOrPercent, ptFFF);
    def->label = L("Skirt");
    def->full_label = L("Skirt width");
    def->category = OptionCategory::width;
    def->invalidates_step = psSkirtBrim;
    def->tooltip = L("Horizontal width of the skirt that will be printed around each object."
        " If left as zero, first layer extrusion width will be used if set and the skirt is only 1 layer height"
        ", or perimeter extrusion width will be used (using the computed value if not set).");
    def->sidetext = L("mm or %");
    def->ratio_over = "nozzle_diameter";
    def->min = 0;
    def->max = 1000;
    def->max_literal = { 10, true };
    def->precision = 6;
    def->mode = comAdvancedE | comSuSi;
    def->set_default_value(new ConfigOptionFloatOrPercent(130, true));

    def = definition.add("skirts", coInt, ptFFF);
    def->label = L("Loops (minimum)");
    def->full_label = L("Skirt Loops");
    def->category = OptionCategory::skirtBrim;
    def->invalidates_step = psSkirtBrim;
    def->tooltip = L("Number of loops for the skirt. If the Minimum Extrusion Length option is set, "
                   "the number of loops might be greater than the one configured here. Set zero "
                   "to disable skirt completely.");
    def->min = 0;
    def->mode = comSimpleAE | comPrusa;
    def->set_default_value(new ConfigOptionInt(1));

    def = definition.add("slice_closing_radius", coFloat, ptFFF);
    def->label = L("Slice gap closing radius");
    def->category = OptionCategory::slicing;
    def->invalidates_step = posSlice;
    def->tooltip = L("Cracks smaller than 2x gap closing radius are being filled during the triangle mesh slicing. "
        "The gap closing operation may reduce the final print resolution, therefore it is advisable to keep the value reasonably low.");
    def->sidetext = L("mm");
    def->min = 0;
    def->precision = 8;
    def->mode = comAdvancedE | comPrusa;
    def->set_default_value(new ConfigOptionFloat(0.049));

    def = definition.add("slice_merge_dent", coFloatOrPercent, ptFFF);
    def->label = L("Merge mmu with a dent");
    def->full_label = L("Slice mmu merge: dent");
    def->category = OptionCategory::slicing;
    def->invalidates_step = posSlice;
    def->tooltip = L("When you have in an object multiple parts,"
        " the last one in the list has the highest priority and will be used where it intersects other parts."
        " This setting only works when the two parts are each assigned to a different extruder."
        " This setting allow the other parts to keep a little bit of their former surface by a certain amount."
        "\nCan be a mm or a % of the external perimeter width");
    def->sidetext = L("mm or %");
    def->min = 0;
    def->mode = comAdvancedE | comSuSi;
    def->set_default_value(new ConfigOptionFloatOrPercent(0, false));

    def = definition.add("slice_merge_min_width", coFloatOrPercent, ptFFF);
    def->label = L("Merge mmu with minimum width");
    def->full_label = L("Slice mmu merge: min width");
    def->category = OptionCategory::slicing;
    def->invalidates_step = posSlice;
    def->tooltip = L("When you have in an object multiple parts,"
        " the last one in the list has the highest priority and will be used where it intersects other parts."
        " This setting only works when the two parts are each assigned to a different extruder."
        " This setting allow to collapse first thin areas of the part before removing it from the other parts,"
        " as doing this can create holes without anything printed inside, as it's too thin."
        "\nCan be a mm or a % of the external perimeter width");
    def->sidetext = L("mm or %");
    def->min = 0;
    def->mode = comAdvancedE | comSuSi;
    def->set_default_value(new ConfigOptionFloatOrPercent(120, true));

    def = definition.add("slicing_mode", coEnum, ptFFF);
    def->label = L("Slicing Mode");
    def->category = OptionCategory::slicing;
    def->invalidates_step = posSlice;
    def->tooltip = L("Use \"Even-odd\" for 3DLabPrint airplane models. Use \"Close holes\" to close all holes in the model.");
    def->set_enum<SlicingMode>({
        { "regular",        L("Regular") },
        { "even_odd",       L("Even-odd") },
        { "close_holes",    L("Close holes") }
    });
    def->mode = comAdvancedE | comPrusa;
    def->set_default_value(new ConfigOptionEnum<SlicingMode>(SlicingMode::Regular));

    def = definition.add("slowdown_below_layer_time", coFloats, ptFFF);
    def->label = L("Slow down if layer print time is below");
    def->category = OptionCategory::cooling;
    def->invalidates_step = psGCodeExport;
    def->tooltip = L("If layer print time is estimated below this number of seconds, print moves "
        "speed will be scaled down to extend duration to this value, if possible."
        "\nSet zero to disable.");
    def->sidetext = L("approximate seconds");
    def->min = 0;
    def->max = 1000;
    def->mode = comExpert | comPrusa;
    def->is_vector_extruder = true;
    def->set_default_value(new ConfigOptionFloats{ 5 });

    def = definition.add("small_perimeter_speed", coFloatOrPercent, ptFFF);
    def->label = L("Speed");
    def->full_label = L("Small perimeters speed");
    def->category = OptionCategory::speed;
    def->invalidates_step = psGCodeExport;
    def->tooltip = L("This separate setting will affect the speed of perimeters having radius <= 6.5mm (usually holes)."
                   "\nIf expressed as percentage (for example: 80%) it will be calculated on the Internal Perimeters speed setting above."
                   "\nSet zero to disable.");
    def->sidetext = L("mm/s or %");
    def->ratio_over = "perimeter_speed";
    def->min = 0;
    def->mode = comAdvancedE | comPrusa;
    def->set_default_value(new ConfigOptionFloatOrPercent(50, true));

    def = definition.add("small_perimeter_min_length", coFloatOrPercent, ptFFF);
    def->label = L("Min length");
    def->full_label = L("Min small perimeters length");
    def->category = OptionCategory::speed;
    def->invalidates_step = psGCodeExport;
    def->tooltip = L("This sets the threshold for small perimeter length. Every loop with a length lower than this will be printed at small perimeter speed"
        "\nCan be a mm value or a % of the nozzle diameter.");
    def->sidetext = L("mm or %");
    def->ratio_over = "nozzle_diameter";
    def->min = 0;
    def->max_literal = { 100, false };
    def->mode = comAdvancedE | comSuSi;
    def->set_default_value(new ConfigOptionFloatOrPercent(6, false));


    def = definition.add("small_perimeter_max_length", coFloatOrPercent, ptFFF);
    def->label = L("Max length");
    def->full_label = L("Max small perimeters length");
    def->category = OptionCategory::speed;
    def->invalidates_step = psGCodeExport;
    def->tooltip = L("This sets the end of the threshold for small perimeter length."
        " Every perimeter loop lower than this will see their speed reduced a bit, from their normal speed at this length down to small perimeter speed."
        "\nCan be a mm or a % of the nozzle diameter.");
    def->sidetext = L("mm or %");
    def->ratio_over = "nozzle_diameter";
    def->min = 0;
    def->max_literal = { 500, false };
    def->mode = comAdvancedE | comSuSi;
    def->set_default_value(new ConfigOptionFloatOrPercent(20, false));

    def = definition.add("solid_infill_below_area", coFloat, ptFFF);
    def->label = L("Solid infill threshold area");
    def->category = OptionCategory::infill;
    def->invalidates_step = posPrepareInfill;
    def->tooltip = L("Force solid infill for regions having a smaller area than the specified threshold.");
    def->sidetext = L("mm²");
    def->min = 0;
    def->mode = comExpert | comPrusa;
    def->set_default_value(new ConfigOptionFloat(4));

    def = definition.add("solid_infill_below_layer_area", coFloat, ptFFF);
    def->label = L("Solid infill layer threshold area");
    def->category = OptionCategory::infill;
    def->invalidates_step = posPrepareInfill;
    def->tooltip = L("Force solid infill for the whole layer when the combined area of all objects that are printed at the same layer is smaller than this value.");
    def->sidetext = L("mm²");
    def->min = 0;
    def->mode = comExpert | comSuSi;
    def->set_default_value(new ConfigOptionFloat(0));

    def = definition.add("solid_infill_below_width", coFloatOrPercent, ptFFF);
    def->label = L("Solid infill threshold width");
    def->category = OptionCategory::infill;
    def->invalidates_step = posPrepareInfill;
    def->tooltip = L("Force solid infill for parts of regions having a smaller width than the specified threshold."
                    "\nCan be a % of the current solid infill spacing."
                    "\nSet 0 to disable.");
    def->sidetext = L("mm or %");
    def->min = 0;
    def->mode = comExpert | comSuSi;
    def->set_default_value(new ConfigOptionFloatOrPercent(0, false));

    def = definition.add("solid_infill_overlap", coPercent, ptFFF);
    def->label = L("Solid infill overlap");
    def->category = OptionCategory::width;
    def->invalidates_step = posPerimeters;
    def->tooltip = L("This setting allows you to reduce the overlap between the lines of the solid fill, to reduce the % filled if you see overextrusion signs on solid areas."
        " Note that you should be sure that your flow (filament extrusion multiplier) is well calibrated and your filament max overlap is set before thinking to modify this."
        "\nNote: top surfaces are still extruded with 100% overlap to prevent gaps.");
    def->sidetext = L("%");
    def->min = 0;
    def->max = 100;
    def->mode = comExpert | comSuSi;
    def->set_default_value(new ConfigOptionPercent(100));

    def = definition.add("solid_infill_extruder", coInt, ptFFF);
    def->label = L("Solid infill extruder");
    def->category = OptionCategory::extruders;
    def->invalidates_step = posPrepareInfill;
    def->tooltip = L("The extruder to use when printing solid infill.");
    def->min = 1;
    def->mode = comAdvancedE | comPrusa;
    def->set_default_value(new ConfigOptionInt(1));

    def = definition.add("solid_infill_every_layers", coInt, ptFFF);
    def->label = L("Solid infill every");
    def->category = OptionCategory::infill;
    def->invalidates_step = posPrepareInfill;
    def->tooltip = L("This feature allows you to force a solid layer every given number of layers. "
                   "Zero to disable. You can set this to any value (for example 9999); "
                   "Slic3r will automatically choose the maximum possible number of layers "
                   "to combine according to nozzle diameter and layer height.");
    def->sidetext = L("layers");
    def->min = 0;
    def->mode = comExpert | comPrusa;
    def->set_default_value(new ConfigOptionInt(0));

    def = definition.add("solid_infill_extrusion_width", coFloatOrPercent, ptFFF);
    def->label = L("Solid infill");
    def->full_label = L("Solid infill width");
    def->category = OptionCategory::width;
    def->invalidates_step = posPerimeters;
    def->tooltip = L("Set this to a non-zero value to set a manual extrusion width for infill for solid surfaces. "
        "If left as zero, default extrusion width will be used if set, otherwise 1.125 x nozzle diameter will be used. "
        "If expressed as percentage (for example 110%) it will be computed over nozzle diameter."
        "\nYou can set either 'Spacing', or 'Width'; the other will be calculated, using default layer height.");
    def->sidetext = L("mm or %");
    def->ratio_over = "nozzle_diameter";
    def->min = 0;
    def->max = 1000;
    def->max_literal = { 10, true };
    def->precision = 6;
    def->can_phony = true;
    def->mode = comAdvancedE | comPrusa;
    def->set_default_value((new ConfigOptionFloatOrPercent(0, false))->set_phony(true));

    def = definition.add("solid_infill_extrusion_change_odd_layers", coFloatOrPercent, ptFFF);
    def->label = L("Infill");
    def->full_label = L("Solid infill spacing change on even layers");
    def->category = OptionCategory::width;
    def->invalidates_step = posPerimeters;
    def->tooltip = L("Change width on every even layer (and not on odd layers like the first one) for better overlap with adjacent layers and getting stringer shells. "
        "Try values about +/- 0.1 with different sign."
        "\nThis could be combined with extra permeters on even layers."
        "\nWorks as absolute spacing or a % of the spacing."
        "\nset 0 to disable");
    def->sidetext = L("mm or %");
    def->mode = comExpert | comSuSi;
    def->set_default_value(new ConfigOptionFloatOrPercent(false, 0));

    def = definition.add("solid_infill_extrusion_spacing", coFloatOrPercent, ptFFF);
    def->label = L("Solid spacing");
    def->full_label = L("Solid infill spacing");
    def->category = OptionCategory::width;
    def->invalidates_step = posPerimeters;
    def->tooltip = L("Like Solid infill width but spacing is the distance between two lines (as they overlap a bit, it's not the same)."
        "\nYou can set either 'Spacing', or 'Width'; the other will be calculated, using default layer height.");
    def->sidetext = L("mm or %");
    def->ratio_over = "nozzle_diameter";
    def->min = 0;
    def->max = 1000;
    def->max_literal = { 10, true };
    def->precision = 6;
    def->can_phony = true;
    def->mode = comAdvancedE | comSuSi;
    def->set_default_value(new ConfigOptionFloatOrPercent(100, true));

    def = definition.add("solid_infill_fan_speed", coInts, ptFFF);
    def->label = L("Solid Infill fan speed");
    def->category = OptionCategory::cooling;
    def->invalidates_step = psGCodeExport;
    def->tooltip = L("This fan speed is enforced during all Solid Infill moves"
        "\nSet to 0 to stop the fan."
        "\nIf disabled, default fan speed will be used."
        "\nCan be disabled by disable_fan_first_layers, slowed down by full_fan_speed_layer and increased by low layer time.");
    def->sidetext = L("%");
    def->min = 0;
    def->max = 100;
    def->mode = comExpert | comSuSi;
    def->is_vector_extruder = true;
    def->can_be_disabled = true;
    def->set_default_value(disable_default_option(new ConfigOptionInts({ 100 })));

    def = definition.add("solid_infill_speed", coFloatOrPercent, ptFFF);
    def->label = L("Solid");
    def->full_label = L("Solid infill speed");
    def->category = OptionCategory::speed;
    def->invalidates_step = psGCodeExport;
    def->tooltip = L("Speed for printing solid regions (top/bottom/internal horizontal shells). "
        "\nThis can be expressed as a percentage (for example: 80%) over the Default speed."
        "\nSet zero to use autospeed for this feature.");
    def->sidetext = L("mm/s or %");
    def->ratio_over = "default_speed";
    def->aliases = { "solid_infill_feed_rate" };
    def->min = 0;
    def->mode = comAdvancedE | comPrusa;
    def->set_default_value(new ConfigOptionFloatOrPercent(30, true));

#if 0
    def = definition.add("solid_layers", coInt, ptFFF);
    def->label = L("Solid layers");
    def->category = OptionCategory::slicing;
    def->tooltip = L("Number of solid layers to generate on top and bottom surfaces.");
    def->shortcut.push_back("top_solid_layers");
    def->shortcut.push_back("bottom_solid_layers");
    def->min = 0;
    def->mode = comSimpleAE | comPrusa;

    def = definition.add("solid_min_thickness", coFloat, ptFFF);
    def->label = L("Minimum thickness of a top / bottom shell");
    def->tooltip = L("Minimum thickness of a top / bottom shell");
    def->shortcut.push_back("top_solid_min_thickness");
    def->shortcut.push_back("bottom_solid_min_thickness");
    def->min = 0;
    def->mode = comSimpleAE | comPrusa;
#endif

    def = definition.add("spiral_vase", coBool, ptFFF);
    def->label = L("Spiral vase");
    def->category = OptionCategory::perimeter;
    def->invalidates_step = posSlice;
    def->tooltip = L("This feature will raise Z gradually while printing a single-walled object "
                   "in order to remove any visible seam. This option requires "
                   "no infill, no top solid layers and no support material. You can still set "
                   "any number of bottom solid layers as well as skirt/brim loops."
                   " After the bottom solid layers, the number of perimeters is enforce to 1."
                   "It won't work when printing more than one single object.");
    def->mode = comSimpleAE | comPrusa;
    def->set_default_value(new ConfigOptionBool(false));

    def = definition.add("standby_temperature_delta", coInt, ptFFF);
    def->label = L("Temperature variation");
    def->invalidates_step = psGCodeExport;
    // TRN PrintSettings : "Ooze prevention" > "Temperature variation"
    def->tooltip = L("Temperature difference to be applied when an extruder is not active. "
                     "The value is not used when 'idle_temperature' in filament settings "
                     "is defined.");
    def->sidetext = "∆°C";
    def->min = -max_temp;
    def->max = max_temp;
    def->mode = comExpert | comPrusa;
    def->set_default_value(new ConfigOptionInt(-5));

    def = definition.add("autoemit_temperature_commands", coBool, ptFFF);
    def->label = L("Emit temperature commands automatically");
    def->invalidates_step = psGCodeExport;
    def->tooltip = L("When enabled, Slic3r will check whether your Custom Start G-Code contains M104 or M190. "
                     "If so, the temperatures will not be emitted automatically so you're free to customize "
                     "the order of heating commands and other custom actions. Note that you can use "
                     "placeholder variables for all Slic3r settings, so you can put "
                     "a \"M109 S[first_layer_temperature]\" command wherever you want.\n"
                     "If your Custom Start G-Code does NOT contain M104 or M190, "
                     "Slic3r will execute the Start G-Code after bed reached its target temperature "
                     "and extruder just started heating.\n\n"
                     "When disabled, Slic3r will NOT emit commands to heat up extruder and bed, "
                     "leaving both to Custom Start G-Code.");
    def->mode = comExpert | comPrusa;
    def->set_default_value(new ConfigOptionBool(true));

    def = definition.add("start_gcode", coString, ptFFF);
    def->label = L("Start G-code");
    def->category = OptionCategory::customgcode;
    def->invalidates_step = psGCodeExport;
    def->tooltip = L("This start procedure is inserted at the beginning, possibly prepended by "
                     "temperature-changing commands and others. See 'autoemit_temperature_commands' and 'start_gcode_manual'.");
    def->multiline = true;
    def->full_width = true;
    def->height = 12;
    def->mode = comExpert | comPrusa;
    def->set_default_value(new ConfigOptionString("G28 ; home all axes\nG1 Z5 F5000 ; lift nozzle\n"));

    def = definition.add("start_gcode_manual", coBool, ptFFF);
    def->label = L("Only custom Start G-code");
    def->category = OptionCategory::customgcode;
    def->invalidates_step = psGCodeExport;
    def->tooltip = L("Ensure that the slicer won't add heating, fan, extruder... commands before or just after your start-gcode."
                    "\nIf set to true, you have to write a good and complete start_gcode, as no checks are made anymore."
                    "\nExemple:\nG21 ; set units to millimeters\nG90 ; use absolute coordinates\n{if use_relative_e_distances}M83{else}M82{endif}\nG92 E0 ; reset extrusion distance");
    def->mode = comExpert | comSuSi;
    def->set_default_value(new ConfigOptionBool(false));

    def = definition.add("start_filament_gcode", coStrings, ptFFF);
    def->label = L("Start G-code");
    def->full_label = L("Filament start G-code");
    def->category = OptionCategory::customgcode;
    def->invalidates_step = psGCodeExport;
    def->tooltip = L("This start procedure is inserted at the beginning, after any printer start gcode (and "
                   "after any toolchange to this filament in case of multi-material printers). "
                   "This is used to override settings for a specific filament. If Slic3r detects "
                   "M104, M109, M140 or M190 in your custom codes, such commands will "
                   "not be prepended automatically so you're free to customize the order "
                   "of heating commands and other custom actions. Note that you can use placeholder variables "
                   "for all Slic3r settings, so you can put a \"M109 S{first_layer_temperature}\" command "
                   "wherever you want. If you have multiple extruders, the gcode is processed "
                   "in extruder order.");
    def->multiline = true;
    def->full_width = true;
    def->height = 12;
    def->mode = comExpert | comPrusa;
    def->is_vector_extruder = true;
    def->set_default_value(new ConfigOptionStrings { "; Filament gcode\n" });

    def = definition.add("color_change_gcode", coString, ptFFF);
    def->label = L("Color change G-code");
    def->invalidates_step = psGCodeExport;
    def->tooltip = L("This G-code will be used as a code for the color change"
                     " If empty, the default color change print command for the selected G-code flavor will be used (if any).");
    def->multiline = true;
    def->full_width = true;
    def->height = 12;
    def->mode = comExpert | comPrusa;
    def->set_default_value(new ConfigOptionString(""));

    def = definition.add("parallel_objects_step", coFloat, ptFFF);
    def->label = L("Parallel printing step");
    def->category = OptionCategory::output;
    def->invalidates_step = psGCodeExport;
    def->tooltip = L("When multiple objects are present, instead of jumping from one to another at each layer"
        " the printer will continue to print the current object layers up to this height before moving to the next object."
        " (first layers will be still printed one by one)."
        "\nThis feature also use the same extruder clearance radius field as 'complete individual objects' (complete_objects)"
        ", but you can modify them to instead reflect the clerance of the nozzle, if this field reflect the z-clearance of it."
        "\nThis field is exclusive with 'complete individual objects' (complete_objects). Set to 0 to deactivate.");
    def->sidetext = L("mm");
    def->mode = comAdvancedE | comSuSi;
    def->set_default_value(new ConfigOptionFloat(0));

    def = definition.add("parallel_objects_step_max_z", coFloat, ptFFF);
    def->label = L("Max height for parallel printing step");
    def->category = OptionCategory::output;
    def->invalidates_step = psGCodeExport;
    def->tooltip = L("If the nozzle print higher than taht, the print is switched back to normal printing. Allow to quicly print the first layer per object if these need quick printing.");
    def->sidetext = L("mm");
    def->mode = comAdvancedE | comSuSi;
    def->set_default_value(new ConfigOptionFloat(0));

    def = definition.add("parallel_islands", coBool, ptFFF);
    def->label = L("Island Parallel printing step");
    def->category = OptionCategory::output;
    def->invalidates_step = psGCodeExport;
    def->tooltip = L("When using 'parallel_objects_step', consider each object island as a separate object, if far enough."
                    "\nTwo islands are considered separate if they are farther than the extruder clearance.");
    def->sidetext = L("mm");
    def->mode = comAdvancedE | comSuSi;
    def->set_default_value(new ConfigOptionBool(false));

    def = definition.add("pause_print_gcode", coString, ptFFF);
    def->label = L("Pause Print G-code");
    def->invalidates_step = psGCodeExport;
    def->tooltip = L("This G-code will be used as a code for the pause print."
                    " If empty, the default pause print command for the selected G-code flavor will be used (if any).");
    def->multiline = true;
    def->full_width = true;
    def->height = 12;
    def->mode = comExpert | comPrusa;
    def->set_default_value(new ConfigOptionString(""));

    def = definition.add("template_custom_gcode", coString, ptFFF);
    def->label = L("Custom G-code");
    def->invalidates_step = psGCodeExport;
    def->tooltip = L("This G-code will be used as a custom code");
    def->multiline = true;
    def->full_width = true;
    def->height = 12;
    def->mode = comExpert | comPrusa;
    def->set_default_value(new ConfigOptionString(""));

    def = definition.add("single_extruder_multi_material", coBool, ptFFF);
    def->label = L("Single Extruder Multi Material");
    def->category = OptionCategory::mmsetup;
    def->invalidates_step = psSkirtBrim;
    def->tooltip = L("The printer multiplexes filaments into a single hot end.");
    def->mode = comAdvancedE | comPrusa;
    def->set_default_value(new ConfigOptionBool(false));

    def = definition.add("single_extruder_multi_material_priming", coBool, ptFFF);
    def->label = L("Prime all printing extruders");
    def->category = OptionCategory::mmsetup;
    def->invalidates_step = psGCodeExport;
    def->tooltip = L("If enabled, all printing extruders will be primed at the front edge of the print bed at the start of the print.");
    def->mode = comAdvancedE | comPrusa;
    def->set_default_value(new ConfigOptionBool(true));

    def = definition.add("solid_infill_acceleration", coFloatOrPercent, ptFFF);
    def->label = L("Solid ");
    def->full_label = L("Solid acceleration");
    def->category = OptionCategory::speed;
    def->invalidates_step = psGCodeExport;
    def->tooltip = L("This is the acceleration your printer will use for solid infill. "
                "\nCan be a % of the default acceleration"
                "\nSet zero or 100% to use default acceleration for solid infill.");
    def->sidetext = L("mm/s² or %");
    def->ratio_over = "default_acceleration";
    def->min = 0;
    def->max_literal = { -200, false };
    def->mode = comAdvancedE | comPrusa;
    def->set_default_value(new ConfigOptionFloatOrPercent(0,false));

    def = definition.add("solid_over_perimeters", coInt, ptFFF);
    def->label = L("No solid infill over");
    def->full_label = L("No solid infill over perimeters");
    def->sidetext = L("perimeters");
    def->sidetext_width = 20;
    def->category = OptionCategory::perimeter;
    def->invalidates_step = posPrepareInfill;
    def->tooltip = L("In sloping areas, when you have a number of top / bottom solid layers and few perimeters, "
        " it may be necessary to put some solid infill above/below the perimeters to fulfill the top/bottom layers criteria."
        "\nBy setting this to something higher than 0, you can control this behaviour, which might be desirable if "
        "\nundesirable solid infill is being generated on slopes."
        "\nThe number set here indicates the number of layers between the inside of the part and the air"
        " at and beyond which solid infill should no longer be added above/below. If this setting is equal or higher than "
        " the top/bottom solid layer count, it won't do anything. If this setting is set to 1, it will evict "
        " all solid fill above/below perimeters. "
        "\nSet zero to disable.");
    def->min = 0;
    def->mode = comAdvancedE | comSuSi;
    def->set_default_value(new ConfigOptionInt(0));

    def = definition.add("support_material", coBool, ptFFF);
    def->label = L("Generate support material");
    def->category = OptionCategory::support;
    def->invalidates_step = posSlice;
    def->tooltip = L("Enable support material generation.");
    def->mode = comSimpleAE | comPrusa;
    def->set_default_value(new ConfigOptionBool(false));

    def = definition.add("support_material_acceleration", coFloatOrPercent, ptFFF);
    def->label = L("Default");
    def->full_label = L("Support acceleration");
    def->category = OptionCategory::speed;
    def->invalidates_step = psGCodeExport;
    def->tooltip = L("This is the acceleration your printer will use for support material. "
                "\nCan be a % of the default acceleration"
                "\nSet zero to use default acceleration for support material.");
    def->sidetext = L("mm/s² or %");
    def->ratio_over = "default_acceleration";
    def->min = 0;
    def->max_literal = { -200, false };
    def->mode = comAdvancedE | comSuSi;
    def->set_default_value(new ConfigOptionFloatOrPercent(0,false));

    def = definition.add("support_material_auto", coBool, ptFFF);
    def->label = L("Auto generated supports");
    def->category = OptionCategory::support;
    def->invalidates_step = posSupportMaterial;
    def->tooltip = L("If checked, supports will be generated automatically based on the overhang threshold value."\
                     " If unchecked, supports will be generated inside the \"Support Enforcer\" volumes only.");
    def->mode = comSimpleAE | comPrusa;
    def->set_default_value(new ConfigOptionBool(true));

    def = definition.add("support_material_interface_acceleration", coFloatOrPercent, ptFFF);
    def->label = L("Interface");
    def->full_label = L("Support interface acceleration");
    def->category = OptionCategory::speed;
    def->invalidates_step = psGCodeExport;
    def->tooltip = L("This is the acceleration your printer will use for support material interfaces. "
                "\nCan be a % of the support material acceleration"
                "\nSet zero to use support acceleration for support material interfaces.");
    def->sidetext = L("mm/s² or %");
    def->ratio_over = "support_material_acceleration";
    def->min = 0;
    def->max_literal = { -200, false };
    def->mode = comExpert | comSuSi;
    def->set_default_value(new ConfigOptionFloatOrPercent(0,false));

    def = definition.add("support_material_xy_spacing", coFloatOrPercent, ptFFF);
    def->label = L("XY separation between an object and its support");
    def->category = OptionCategory::support;
    def->invalidates_step = posSupportMaterial;
    def->tooltip = L("XY separation between an object and its support. If expressed as percentage "
                   "(for example 50%), it will be calculated over external perimeter width.");
    def->sidetext = L("mm or %");
    def->ratio_over = "external_perimeter_extrusion_width";
    def->min = 0;
    def->max_literal = { 10, false};
    def->mode = comAdvancedE | comPrusa;
    // Default is half the external perimeter width.
    def->set_default_value(new ConfigOptionFloatOrPercent(50, true));

    def = definition.add("support_material_angle", coFloat, ptFFF);
    def->label = L("Pattern angle");
    def->full_label = L("Support pattern angle");
    def->category = OptionCategory::support;
    def->invalidates_step = posSupportMaterial;
    def->tooltip = L("Use this setting to rotate the support material pattern on the horizontal plane.");
    def->sidetext = L("°");
    def->min = 0;
    def->max = 359;
    def->mode = comExpert | comPrusa;
    def->set_default_value(new ConfigOptionFloat(0));

    def = definition.add("support_material_angle_height", coFloat, ptFFF);
    def->label = L("Pattern angle swap height");
    def->category = OptionCategory::support;
    def->invalidates_step = posSupportMaterial;
    def->tooltip = L("Use this setting to rotate the support material pattern by 90° at this height (in mm). Set 0 to disable.");
    def->sidetext = L("mm");
    def->min = 0;
    def->mode = comExpert | comSuSi;
    def->set_default_value(new ConfigOptionFloat(0));

    def = definition.add("support_material_buildplate_only", coBool, ptFFF);
    def->label = L("Support on build plate only");
    def->category = OptionCategory::support;
    def->invalidates_step = posSupportMaterial;
    def->tooltip = L("Only create support if it lies on a build plate. Don't create support on a print.");
    def->mode = comSimpleAE | comPrusa;
    def->set_default_value(new ConfigOptionBool(false));

    def = definition.add("support_material_contact_distance_type", coEnum, ptFFF);
    def->label = L("Type");
    def->full_label = L("Support contact distance type");
    def->category = OptionCategory::support;
    def->invalidates_step = posSlice;
    def->tooltip = L("How to compute the vertical z-distance.\n"
        "From filament: it uses the nearest bit of the filament. When a bridge is extruded, it goes below the current plane.\n"
        "From plane: it uses the plane-z. Same as 'from filament' if no 'bridge' is extruded.\n"
        "None: No z-offset. Useful for Soluble supports.\n");
    def->set_enum<SupportZDistanceType>({
        { "filament", L("From filament") },
        { "plane",    L("From plane") },
        { "none",     L("None (soluble)") }
    });
    def->mode = comAdvancedE | comSuSi;
    def->set_default_value(new ConfigOptionEnum<SupportZDistanceType>(zdFilament));

    def = definition.add("support_material_contact_distance", coFloatOrPercent, ptFFF);
    def->label = L("Top");
    def->full_label = L("Contact distance on top of supports");
    def->category = OptionCategory::support;
    def->invalidates_step = posSlice;
    def->tooltip = L("The vertical distance between support material interface and the object"
        "(when the object is printed on top of the support). "
        "Setting this to 0 will also prevent Slic3r from using bridge flow and speed "
        "for the first object layer. Can be a % of the nozzle diameter.");
    def->ratio_over = "nozzle_diameter";
    def->sidetext = L("mm");
    def->min = 0;
    def->max_literal = { 20, true };
    def->mode = comAdvancedE | comPrusa;
    def->aliases = { "support_material_contact_distance_top" }; // Sli3r, PS
    def->set_default_value(new ConfigOptionFloatOrPercent(0.2, false));

    def = definition.add("support_material_bottom_contact_distance", coFloatOrPercent, ptFFF);
    def->label = L("Bottom");
    def->full_label = L("Contact distance under the bottom of supports");
    def->category = OptionCategory::support;
    def->invalidates_step = posSlice;
    def->tooltip = L("The vertical distance between object and support material interface"
        "(when the support is printed on top of the object). Can be a % of the nozzle diameter."
        "\nIf set to zero, support_material_contact_distance will be used for both top and bottom contact Z distances.");
    def->ratio_over = "nozzle_diameter";
    def->sidetext = L("mm");
    def->set_enum_values(ConfigOptionDef::GUIType::f_enum_open, {
    //TRN Print Settings: "Bottom contact Z distance". Have to be as short as possible
        { "0",      L("Same as top") },
        { "0.1",    "0.1" },
        { "0.2",    "0.2" },
        { "50%",    "50%" },
    });
    def->min = 0;
    def->max_literal = { 20, true };
    def->mode = comAdvancedE | comPrusa;
    def->aliases = { "support_material_contact_distance_bottom" }; //since PS 2.4
    def->set_default_value(new ConfigOptionFloatOrPercent(0.2,false));

    def = definition.add("support_material_bottom_interface_expansion", coFloatOrPercent, ptFFF);
    def->label = L("Bottom interface expansion");
    def->category = OptionCategory::support;
    def->invalidates_step = posSupportMaterial;
    def->tooltip = L("Expanion of the bottom interface for better stability."
        "\nCan be percentage of the interface line width.");
    def->sidetext = L("mm or %");
    def->min = 0;
    def->mode = comAdvancedE | comSuSi;
    def->set_default_value(new ConfigOptionFloatOrPercent(100, true));

    def = definition.add("support_material_enforce_layers", coInt, ptFFF);
    def->label = L("Enforce support for the first");
    def->category = OptionCategory::support;
    def->invalidates_step = posSupportMaterial;
    def->tooltip = L("Generate support material for the specified number of layers counting from bottom, "
                   "regardless of whether normal support material is enabled or not and regardless "
                   "of any angle threshold. This is useful for getting more adhesion of objects "
                   "having a very thin or poor footprint on the build plate.");
    def->sidetext = L("layers");
    def->full_label = L("Enforce support for the first n layers");
    def->min = 0;
    def->mode = comExpert | comPrusa;
    def->set_default_value(new ConfigOptionInt(0));

    def = definition.add("support_material_extruder", coInt, ptFFF);
    def->label = L("Support material extruder");
    def->category = OptionCategory::extruders;
    def->invalidates_step = posSupportMaterial;
    def->tooltip = L("The extruder to use when printing support material "
                   "(1+, 0 to use the current extruder to minimize tool changes).");
    def->min = 0;
    def->mode = comAdvancedE | comPrusa;
    def->set_default_value(new ConfigOptionInt(0));

    def = definition.add("support_material_extrusion_width", coFloatOrPercent, ptFFF);
    def->label = L("Support material");
    def->full_label = L("Support material width");
    def->category = OptionCategory::width;
    def->invalidates_step = posSupportMaterial;
    def->tooltip = L("Set this to a non-zero value to set a manual extrusion width for support material. "
        "If left as zero, default extrusion width will be used if set, otherwise nozzle diameter will be used. "
        "If expressed as percentage (for example 110%) it will be computed over nozzle diameter.");
    def->sidetext = L("mm or %");
    def->ratio_over = "nozzle_diameter";
    def->min = 0;
    def->max = 1000;
    def->max_literal = { 10, true };
    def->precision = 6;
    def->mode = comAdvancedE | comPrusa;
    def->set_default_value(new ConfigOptionFloatOrPercent(0, false));

    def = definition.add("support_material_fan_speed", coInts, ptFFF);
    def->label = L("Support Material fan speed");
    def->category = OptionCategory::cooling;
    def->invalidates_step = psGCodeExport;
    def->tooltip = L("This fan speed is enforced during all support moves"
        "\nSet to 0 to stop the fan."
        "\nIf disabled, default fan speed will be used."
        "\nCan be disabled by disable_fan_first_layers, slowed down by full_fan_speed_layer.");
    def->sidetext = L("%");
    def->min = 0;
    def->max = 100;
    def->mode = comExpert | comSuSi;
    def->is_vector_extruder = true;
    def->can_be_disabled = true;
    def->set_default_value(disable_default_option(new ConfigOptionInts({ 100 })));

    def = definition.add("support_material_interface_angle", coFloat, ptFFF);
    def->label = L("Pattern angle");
    def->full_label = L("Support interface pattern angle");
    def->category = OptionCategory::support;
    def->invalidates_step = posSupportMaterial;
    def->tooltip = L("Use this setting to rotate the support material pattern on the horizontal plane.\n0 to use the support_material_angle.");
    def->sidetext = L("°");
    def->min = 0;
    def->max = 360;
    def->mode = comExpert | comSuSi;
    def->set_default_value(new ConfigOptionFloat(90));

    def = definition.add("support_material_interface_angle_increment", coFloat, ptFFF);
    def->label = L("Support interface angle increment");
    def->category = OptionCategory::support;
    def->invalidates_step = posSupportMaterial;
    def->tooltip = L("Each layer, add this angle to the interface pattern angle. 0 to keep the same angle, 90 to cross.");
    def->sidetext = L("°");
    def->min = 0;
    def->max = 360;
    def->mode = comAdvancedE | comSuSi;
    def->set_default_value(new ConfigOptionFloat(0));

    def = definition.add("support_material_interface_fan_speed", coInts, ptFFF);
    def->label = L("Support interface fan speed");
    def->category = OptionCategory::cooling;
    def->invalidates_step = psGCodeExport;
    def->tooltip = L("This fan speed is enforced during all support interfaces, to be able to weaken their bonding with a high fan speed."
        "\nSet to 0 to stop the fan."
        "\nIf disabled, Support Material fan speed will be used."
        "\nCan only be overridden by disable_fan_first_layers.");
    def->sidetext = L("%");
    def->min = 0;
    def->max = 100;
    def->mode = comAdvancedE | comSuSi;
    def->is_vector_extruder = true;
    def->can_be_disabled = true;
    def->set_default_value(disable_default_option(new ConfigOptionInts({ 100 })));


    def = definition.add("support_material_interface_contact_loops", coBool, ptFFF);
    def->label = L("Interface loops");
    def->category = OptionCategory::support;
    def->invalidates_step = posSupportMaterial;
    def->tooltip = L("Cover the top contact layer of the supports with loops. Disabled by default.");
    def->mode = comExpert | comPrusa;
    def->set_default_value(new ConfigOptionBool(false));

    def = definition.add("support_material_interface_extruder", coInt, ptFFF);
    def->label = L("Support material/raft interface extruder");
    def->category = OptionCategory::extruders;
    def->invalidates_step = posSupportMaterial;
    def->tooltip = L("The extruder to use when printing support material interface "
        "(1+, 0 to use the current extruder to minimize tool changes). This affects raft too.");
    def->min = 0;
    def->mode = comAdvancedE | comPrusa;
    def->set_default_value(new ConfigOptionInt(0));

    def = definition.add("support_material_interface_layers", coInt, ptFFF);
    def->label = L("Top interface layers");
    def->category = OptionCategory::support;
    def->invalidates_step = posSupportMaterial;
    def->tooltip = L("Number of interface layers to insert between the object(s) and support material.");
    def->sidetext = L("layers");
    def->min = 0;
    def->set_enum_values(ConfigOptionDef::GUIType::i_enum_open, {
        { "0", L("0 (off)") },
        { "1", L("1 (light)") },
        { "2", L("2 (default)") },
        { "3", L("3 (heavy)") }
    });
    def->mode = comAdvancedE | comPrusa;
    def->set_default_value(new ConfigOptionInt(3));

    def = definition.add("support_material_bottom_interface_layers", coInt, ptFFF);
    def->label = L("Bottom interface layers");
    def->category = OptionCategory::support;
    def->invalidates_step = posSupportMaterial;
    def->tooltip = L("Number of interface layers to insert between the object(s) and support material."
        "\nIf disabled, support_material_interface_layers value is used");
    def->sidetext = L("layers");
    def->min = 0;
    def->can_be_disabled = true;
    def->set_enum_values(ConfigOptionDef::GUIType::i_enum_open, {
    //TRN Print Settings: "Bottom interface layers". Have to be as short as possible
        { "0", L("0 (off)") },
        { "1", L("1 (light)") },
        { "2", L("2 (default)") },
        { "3", L("3 (heavy)") }
    });
    def->mode = comAdvancedE | comPrusa;
    def->set_default_value(disable_default_option(new ConfigOptionInt(0)));

    def = definition.add("support_material_closing_radius", coFloat, ptFFF);
    def->label = L("Closing radius");
    def->category = OptionCategory::support;
    def->invalidates_step = posSupportMaterial;
    def->tooltip = L("For snug supports, the support regions will be merged using morphological closing operation."
        " Gaps smaller than the closing radius will be filled in.");
    def->sidetext = L("mm");
    def->min = 0;
    def->mode = comAdvancedE | comPrusa;
    def->set_default_value(new ConfigOptionFloat(2));

    def = definition.add("support_material_interface_layer_height", coFloatOrPercent, ptFFF);
    def->label = L("Support interface layer height");
    def->category = OptionCategory::support;
    def->invalidates_step = posSlice;
    def->tooltip = L("Maximum layer height for the support interface."
        "\nCan be a % of the nozzle diameter"
        "\nIf set to 0, the extruder maximum height will be used.");
    def->sidetext = L("mm or %");
    def->ratio_over = "nozzle_diameter";
    def->min = 0;
    def->mode = comAdvancedE | comSuSi;
    def->set_default_value(new ConfigOptionFloatOrPercent(0, false));

    def = definition.add("support_material_interface_spacing", coFloat, ptFFF);
    def->label = L("Interface pattern spacing");
    def->category = OptionCategory::support;
    def->invalidates_step = posSupportMaterial;
    def->tooltip = L("Spacing between interface lines. Set zero to get a solid interface.");
    def->sidetext = L("mm");
    def->min = 0;
    def->mode = comAdvancedE | comPrusa;
    def->set_default_value(new ConfigOptionFloat(0));

    def = definition.add("support_material_interface_speed", coFloatOrPercent, ptFFF);
    def->label = L("Interface");
    def->full_label = L("Support interface speed");
    def->category = OptionCategory::support;
    def->invalidates_step = psGCodeExport;
    def->tooltip = L("Speed for printing support material interface layers."
        "\nIf expressed as percentage (for example 50%) it will be calculated over support material speed."
        "\nSet zero to use autospeed for this feature.");
    def->sidetext = L("mm/s or %");
    def->ratio_over = "support_material_speed";
    def->min = 0;
    def->mode = comExpert | comPrusa;
    def->set_default_value(new ConfigOptionFloatOrPercent(50, true));

    def = definition.add("support_material_pattern", coEnum, ptFFF);
    def->label = L("Pattern");
    def->full_label = L("Support pattern");
    def->category = OptionCategory::support;
    def->invalidates_step = posSupportMaterial;
    def->tooltip = L("Pattern used to generate support material.");
    def->set_enum<SupportMaterialPattern>({
        { "rectilinear",        L("Rectilinear") },
        { "rectilinear-grid",   L("Rectilinear grid") },
        { "honeycomb",          L("Honeycomb") }
    });
    def->mode = comAdvancedE | comPrusa;
    def->set_default_value(new ConfigOptionEnum<SupportMaterialPattern>(smpRectilinear));

    def = definition.add("support_material_bottom_interface_pattern", coEnum, ptFFF);
    def->label = L("Bottom Pattern");
    def->full_label = L("Support bottom interface pattern");
    def->category = OptionCategory::support;
    def->invalidates_step = posSupportMaterial;
    def->tooltip = L("Pattern for the bottom interface layers (the ones that start on the object)."
        "\nDefault pattern is the same as the top interface, unless it's Hilbert Curve or Ironing."
        "\nNote that 'Hilbert', 'Ironing' , '(filled)' patterns are really discouraged, and meant to be used with soluble supports and 100% fill interface layer.");
    def->set_enum<InfillPattern>({
        { "auto",              L("Default") },
        { "rectilinear",       L("Rectilinear") },
        { "monotonic",         L("Monotonic") },
        { "concentric",        L("Concentric") },
        { "hilbertcurve",      L("Hilbert Curve") },
        { "smooth",            L("Ironing") },
    });
    def->mode = comAdvancedE | comSuSi;
    def->set_default_value(new ConfigOptionEnum<InfillPattern>(ipAuto));

    def = definition.add("support_material_top_interface_pattern", coEnum, ptFFF);
    def->label = L("Top Pattern");
    def->full_label = L("Support top interface pattern");
    def->category = OptionCategory::support;
    def->invalidates_step = posSupportMaterial;
    def->tooltip = L("Pattern for the top interface layers."
        "\nNote that 'Hilbert', 'Ironing' and '(filled)' patterns are meant to be used with soluble supports and 100% fill interface layer.");
    def->set_enum<InfillPattern>({
        { "auto",              L("Default") },
        { "rectilinear",       L("Rectilinear") },
        { "monotonic",         L("Monotonic") },
        { "concentric",        L("Concentric") },
        { "sawtooth",          L("Sawtooth") },
        { "hilbertcurve",      L("Hilbert Curve") },
        { "concentricgapfill", L("Concentric (filled)") },
        { "smooth",            L("Ironing") },
    });
    def->mode = comAdvancedE | comPrusa;
    def->set_default_value(new ConfigOptionEnum<InfillPattern>(ipRectilinear));
    def->aliases = {"support_material_interface_pattern"};

    def = definition.add("support_material_layer_height", coFloatOrPercent, ptFFF);
    def->label = L("Support layer height");
    def->category = OptionCategory::support;
    def->invalidates_step = posSlice;
    def->tooltip = L("Maximum layer height for the support, after the first layer that uses the first layer height, and before the interface layers."
        "\nCan be a % of the nozzle diameter"
        "\nIf set to 0, the extruder maximum height will be used.");
    def->sidetext = L("mm or %");
    def->ratio_over = "nozzle_diameter";
    def->min = 0;
    def->mode = comAdvancedE | comSuSi;
    def->set_default_value(new ConfigOptionFloatOrPercent(0, false));
    
    def = definition.add("support_material_spacing", coFloat, ptFFF);
    def->label = L("Pattern spacing");
    def->category = OptionCategory::support;
    def->invalidates_step = posSupportMaterial;
    def->tooltip = L("Spacing between support material lines.");
    def->sidetext = L("mm");
    def->min = 0;
    def->mode = comAdvancedE | comPrusa;
    def->set_default_value(new ConfigOptionFloat(2.5));

    def = definition.add("support_material_speed", coFloatOrPercent, ptFFF);
    def->label = L("Default");
    def->full_label = L("Support speed");
    def->category = OptionCategory::support;
    def->invalidates_step = psGCodeExport;
    def->tooltip = L("Speed for printing support material."
        "\nThis can be expressed as a percentage (for example: 80%) over the Default speed."
        "\nSet zero to use autospeed for this feature.");
    def->sidetext = L("mm/s or %");
    def->ratio_over = "default_speed";
    def->min = 0;
    def->mode = comAdvancedE | comPrusa;
    def->set_default_value(new ConfigOptionFloatOrPercent(60, true));

    def = definition.add("support_material_style", coEnum, ptFFF);
    def->label = L("Style");
    def->full_label = L("Support tower style");
    def->category = OptionCategory::support;
    def->invalidates_step = posSupportMaterial;
    def->tooltip = L("Style and shape of the support towers. Projecting the supports into a regular grid "
        "will create more stable supports, while snug support towers will save material and reduce "
        "object scarring."
        "\nOrganic: create tree support structure, but this algorithm force to synchronize the support layers with object layers, and only allow for one layer height for each object.");
    def->set_enum<SupportMaterialStyle>({
        { "grid", L("Grid") }, 
        { "snug", L("Snug") },
        { "organic", L("Organic") }
    });
    def->mode = comAdvancedE | comPrusa;
    def->set_default_value(new ConfigOptionEnum<SupportMaterialStyle>(smsGrid));

    def = definition.add("support_material_synchronize_layers", coBool, ptFFF);
    def->label = L("Synchronize with object layers");
    def->category = OptionCategory::support;
    def->invalidates_step = posSupportMaterial;
    // TRN PrintSettings : "Synchronize with object layers"
    def->tooltip = L("Synchronize support layers with the object print layers. This is useful "
                   "with multi-material printers, where the extruder switch is expensive. "
                   "This option is only available when top contact Z distance is set to zero.");
    def->mode = comExpert | comPrusa;
    def->set_default_value(new ConfigOptionBool(false));

    def = definition.add("support_material_threshold", coInt, ptFFF);
    def->label = L("Overhang threshold");
    def->category = OptionCategory::support;
    def->invalidates_step = posSupportMaterial;
    def->tooltip = L("Support material will not be generated for overhangs whose slope angle "
                   "(90° = vertical) is above the given threshold. In other words, this value "
                   "represent the most horizontal slope (measured from the horizontal plane) "
                   "that you can print without support material. Set zero for automatic detection "
                   "(recommended).");
    def->sidetext = L("°");
    def->min = 0;
    def->max = 90;
    def->mode = comAdvancedE | comPrusa;
    def->set_default_value(new ConfigOptionInt(0));

    def = definition.add("support_material_with_sheath", coBool, ptFFF);
    def->label = L("With sheath around the support");
    def->category = OptionCategory::support;
    def->invalidates_step = posSupportMaterial;
    def->tooltip = L("Add a sheath (a single perimeter line) around the base support. This makes "
                   "the support more reliable, but also more difficult to remove.");
    def->mode = comExpert | comPrusa;
    def->set_default_value(new ConfigOptionBool(true));

    def = definition.add("support_tree_angle", coFloat, ptFFF);
    def->label = L("Maximum Branch Angle");
    def->category = OptionCategory::support;
    // TRN PrintSettings: "Organic supports" > "Maximum Branch Angle"
    def->tooltip = L("The maximum angle of the branches, when the branches have to avoid the model. "
                     "Use a lower angle to make them more vertical and more stable. Use a higher angle to be able to have more reach.");
    def->sidetext = L("°");
    def->min = 0;
    def->max = 85;
    def->mode = comAdvancedE | comPrusa;
    def->set_default_value(new ConfigOptionFloat(40));

    def = definition.add("support_tree_angle_slow", coFloat, ptFFF);
    def->label = L("Preferred Branch Angle");
    def->category = OptionCategory::support;
    // TRN PrintSettings: "Organic supports" > "Preferred Branch Angle"
    def->tooltip = L("The preferred angle of the branches, when they do not have to avoid the model. "
                     "Use a lower angle to make them more vertical and more stable. Use a higher angle for branches to merge faster.");
    def->sidetext = L("°");
    def->min = 10;
    def->max = 85;
    def->mode = comAdvancedE | comPrusa;
    def->set_default_value(new ConfigOptionFloat(25));

    def = definition.add("support_tree_tip_diameter", coFloat, ptFFF);
    def->label = L("Tip Diameter");
    def->category = OptionCategory::support;
    // TRN PrintSettings: "Organic supports" > "Tip Diameter"
    def->tooltip = L("Branch tip diameter for organic supports.");
    def->sidetext = L("mm");
    def->min = 0.1f;
    def->max = 100.f;
    def->mode = comAdvancedE | comPrusa;
    def->set_default_value(new ConfigOptionFloat(0.8));

    def = definition.add("support_tree_branch_diameter", coFloat, ptFFF);
    def->label = L("Branch Diameter");
    def->category = OptionCategory::support;
    // TRN PrintSettings: "Organic supports" > "Branch Diameter"
    def->tooltip = L("The diameter of the thinnest branches of organic support. Thicker branches are more sturdy. "
                     "Branches towards the base will be thicker than this.");
    def->sidetext = L("mm");
    def->min = 0.1f;
    def->max = 100.f;
    def->mode = comAdvancedE | comPrusa;
    def->set_default_value(new ConfigOptionFloat(2));

    def = definition.add("support_tree_branch_diameter_angle", coFloat, ptFFF);
    // TRN PrintSettings: #lmFIXME 
    def->label = L("Branch Diameter Angle");
    def->category = OptionCategory::support;
    // TRN PrintSettings: "Organic supports" > "Branch Diameter Angle"
    def->tooltip = L("The angle of the branches' diameter as they gradually become thicker towards the bottom. "
                     "An angle of 0 will cause the branches to have uniform thickness over their length. "
                     "A bit of an angle can increase stability of the organic support.");
    def->sidetext = L("°");
    def->min = 0;
    def->max = 15;
    def->mode = comAdvancedE | comPrusa;
    def->set_default_value(new ConfigOptionFloat(5));

    def = definition.add("support_tree_branch_diameter_double_wall", coFloat, ptFFF);
    def->label = L("Branch Diameter with double walls");
    def->category = OptionCategory::support;
    // TRN PrintSettings: "Organic supports" > "Branch Diameter"
    def->tooltip = L("Branches with area larger than the area of a circle of this diameter will be printed with double walls for stability. "
                     "Set this value to zero for no double walls.");
    def->sidetext = L("mm");
    def->min = 0;
    def->max = 100.f;
    def->mode = comAdvancedE | comPrusa;
    def->set_default_value(new ConfigOptionFloat(3));

    // Tree Support Branch Distance
    // How far apart the branches need to be when they touch the model. Making this distance small will cause 
    // the tree support to touch the model at more points, causing better overhang but making support harder to remove.
    def = definition.add("support_tree_branch_distance", coFloat, ptFFF);
    // TRN PrintSettings: #lmFIXME 
    def->label = L("Branch Distance");
    def->category = OptionCategory::support;
    // TRN PrintSettings: "Organic supports" > "Branch Distance"
    def->tooltip = L("How far apart the branches need to be when they touch the model. "
                     "Making this distance small will cause the tree support to touch the model at more points, "
                     "causing better overhang but making support harder to remove.");
    def->mode = comAdvancedE | comPrusa;
    def->set_default_value(new ConfigOptionFloat(1.));

    def = definition.add("support_tree_top_rate", coPercent, ptFFF);
    def->label = L("Branch Density");
    def->category = OptionCategory::support;
    // TRN PrintSettings: "Organic supports" > "Branch Density"
    def->tooltip = L("Adjusts the density of the support structure used to generate the tips of the branches. "
                     "A higher value results in better overhangs but the supports are harder to remove, "
                     "thus it is recommended to enable top support interfaces instead of a high branch density value "
                     "if dense interfaces are needed.");
    def->sidetext = L("%");
    def->min = 5;
    def->max_literal = {35, false};
    def->mode = comAdvancedE | comPrusa;
    def->set_default_value(new ConfigOptionPercent(15));

    def = definition.add("temperature", coInts, ptFFF);
    def->label = L("Other layers");
    def->full_label = L("Nozzle temperature");
    def->category = OptionCategory::filament;
    def->invalidates_step = psSkirtBrim;
    def->tooltip = L("Extruder nozzle temperature for layers after the first one. Set zero to disable "
                   "temperature control commands in the output G-code.");
    def->sidetext = L("°C");
    def->min = 0;
    def->max = max_temp;
    def->mode = comSimpleAE | comPrusa;
    def->is_vector_extruder = true;
    def->set_default_value(new ConfigOptionInts { 200 });

    def = definition.add("temperature_heat_speed", coFloats, ptFFF);
    def->label = L("Other layers");
    def->full_label = L("Temperature heating speed");
    def->category = OptionCategory::extruders;
    def->invalidates_step = psGCodeExport;
    def->tooltip = L(
        "As the extruder takes time to heat up, when a toolchange is approaching, the next extruder that may be at "
        "parking temp can heat up in advance to be ready for the higher temp more quickly."
        "\nSet to 0 to deactivate.");
    def->sidetext = L("°C/s");
    def->min = 0;
    def->mode = comExpert | comSuSi;
    def->is_vector_extruder = true;
    def->set_default_value(new ConfigOptionFloats { 0 });

    def = definition.add("thin_perimeters", coPercent, ptFFF);
    def->label = L("Overlapping external perimeter");
    def->full_label = L("Overlapping external perimeter");
    def->category = OptionCategory::perimeter;
    def->invalidates_step = posPerimeters;
    def->tooltip = L("Allow outermost perimeter to overlap itself to avoid the use of thin walls. Note that flow isn't adjusted and so this will result in over-extruding and undefined behavior."
                "\n100% means that perimeters can overlap completely on top of each other."
                "\n0% will deactivate this setting."
                "\nValues below 2% don't have any effect."
                "\n-1% will also deactivate the anti-hysteresis checks for external perimeters.");
    def->sidetext = "%";
    def->mode = comExpert | comSuSi;
    def->set_default_value(new ConfigOptionPercent(80));

    def = definition.add("thin_perimeters_all", coPercent, ptFFF);
    def->label = L("Overlapping all perimeters");
    def->full_label = L("Overlapping all perimeters");
    def->category = OptionCategory::perimeter;
    def->invalidates_step = posPerimeters;
    def->tooltip = L("Allow all perimeters to overlap, instead of just external ones."
                "\n100% means that perimeters can overlap completely on top of each other."
                "\n0% will deactivate this setting."
                "\nValues below 2% don't have any effect."
                "\n-1% will also deactivate the anti-hysteresis checks for internal perimeters.");
    def->sidetext = "%";
    def->mode = comExpert | comSuSi;
    def->set_default_value(new ConfigOptionPercent(20));

    def = definition.add("thin_walls", coBool, ptFFF);
    def->label = L("Thin walls");
    def->full_label = L("Thin walls");
    def->category = OptionCategory::perimeter;
    def->invalidates_step = posSlice;
    def->tooltip = L("Detect single-width walls (parts where two extrusions don't fit and we need "
        "to collapse them into a single trace). If unchecked, Slic3r may try to fit perimeters "
        "where it's not possible, creating some overlap leading to over-extrusion.");
    def->mode = comAdvancedE | comPrusa;
    def->set_default_value(new ConfigOptionBool(true));

    def = definition.add("thin_walls_min_width", coFloatOrPercent, ptFFF);
    def->label = L("Min width");
    def->full_label = L("Thin walls min width");
    def->category = OptionCategory::perimeter;
    def->invalidates_step = posPerimeters;
    def->tooltip = L("Minimum width for the extrusion to be extruded (widths lower than the nozzle diameter will be over-extruded at the nozzle diameter)."
        " If expressed as percentage (for example 110%) it will be computed over nozzle diameter."
        " The default behavior of PrusaSlicer is with a 33% value. Put 100% to avoid any sort of over-extrusion.");
    def->ratio_over = "nozzle_diameter";
    def->mode = comExpert | comSuSi;
    def->min = 0;
    def->max_literal = { 20, true };
    def->set_default_value(new ConfigOptionFloatOrPercent(33, true));

    def = definition.add("thin_walls_overlap", coFloatOrPercent, ptFFF);
    def->label = L("Overlap");
    def->full_label = L("Thin wall overlap");
    def->category = OptionCategory::perimeter;
    def->invalidates_step = posPerimeters;
    def->tooltip = L("Overlap between the thin wall and the perimeters. Can be a % of the external perimeter width (default 50%)");
    def->ratio_over = "external_perimeter_extrusion_width";
    def->mode = comExpert | comSuSi;
    def->min = 0;
    def->max_literal = { 10, true };
    def->set_default_value(new ConfigOptionFloatOrPercent(50, true));

    def = definition.add("thin_walls_merge", coBool, ptFFF);
    def->label = L("Merging with perimeters");
    def->full_label = L("Thin wall merge");
    def->category = OptionCategory::perimeter;
    def->invalidates_step = posPerimeters;
    def->tooltip = L("Allow the external perimeter to merge the thin walls in the path."
                    " You can deactivate this if you are using thin walls as a custom support, to reduce adhesion a little.");
    def->mode = comExpert | comSuSi;
    def->set_default_value(new ConfigOptionBool(true));

    def = definition.add("thin_walls_acceleration", coFloatOrPercent, ptFFF);
    def->label = L("Thin Walls");
    def->full_label = L("Thin walls acceleration");
    def->category = OptionCategory::speed;
    def->invalidates_step = psGCodeExport;
    def->tooltip = L("This is the acceleration your printer will use for thin walls. "
                "\nCan be a % of the external perimeter acceleration"
                "\nSet zero to use external perimeter acceleration for thin walls.");
    def->sidetext = L("mm/s² or %");
    def->ratio_over = "external_perimeter_acceleration";
    def->min = 0;
    def->max_literal = { -200, false };
    def->mode = comExpert | comSuSi;
    def->set_default_value(new ConfigOptionFloatOrPercent(0,false));

    def = definition.add("thin_walls_speed", coFloatOrPercent, ptFFF);
    def->label = L("Thin walls");
    def->full_label = L("Thin walls speed");
    def->category = OptionCategory::speed;
    def->invalidates_step = psGCodeExport;
    def->tooltip = L("Speed for thin walls (external extrusions that are alone because the object is too thin at these places)."
        "\nThis can be expressed as a percentage (for example: 80%) over the External Perimeter speed."
        "\nSet zero to use autospeed for this feature.");
    def->sidetext = L("mm/s or %");
    def->ratio_over = "external_perimeter_speed";
    def->min = 0;
    def->mode = comExpert | comSuSi;
    def->set_default_value(new ConfigOptionFloatOrPercent(100, true));

    // put it in Preferences if you want it.
    //def = definition.add("threads", coInt);
    def->invalidates_step = psGCodeExport;
    //def->label = L("Threads");
    //def->tooltip = L("Threads are used to parallelize long-running tasks. Optimal threads number "
    //               "is slightly above the number of available cores/processors.");
    //def->readonly = true;
    //def->min = 1;
    //def->mode = comExpert | comPrusa; // note: hidden setting (and should be a preference)
    //{
    //    int threads = (unsigned int)boost::thread::hardware_concurrency();
    //    def->set_default_value(new ConfigOptionInt(threads > 0 ? threads : 2));
    //    def->cli = ConfigOptionDef::nocli;
    //}

    def = definition.add("time_cost", coFloat, ptFFF);
    def->label = L("Time cost");
    def->category = OptionCategory::firmware;
    def->invalidates_step = psGCodeExport;
    def->tooltip = L("This setting allows you to set how much an hour of printing time is costing you in printer maintenance, loan, human labor, etc.");
    def->mode = comExpert | comSuSi;
    def->sidetext = L("$ per hour");
    def->min = 0;
    def->set_default_value(new ConfigOptionFloat(0));

    def = definition.add("time_estimation_compensation", coPercent, ptFFF);
    def->label = L("Time estimation compensation");
    def->category = OptionCategory::firmware;
    def->invalidates_step = psGCodeExport;
    def->tooltip = L("This setting allows you to modify the time estimation by a % amount. As Slic3r only uses the Marlin algorithm, it's not precise enough if another firmware is used.");
    def->mode = comExpert | comSuSi;
    def->sidetext = L("%");
    def->min = 0;
    def->set_default_value(new ConfigOptionPercent(100));

    def = definition.add("time_start_gcode", coFloat, ptFFF);
    def->label = L("Time for start custom gcode");
    def->category = OptionCategory::firmware;
    def->invalidates_step = psGCodeExport;
    def->tooltip = L("This setting allows you to modify the time estimation by a flat amount to compensate for start script, the homing routine, and other things.");
    def->mode = comExpert | comSuSi;
    def->sidetext = L("s");
    def->min = 0;
    def->set_default_value(new ConfigOptionFloat(20));

    def = definition.add("time_toolchange", coFloat, ptFFF);
    def->label = L("Time for toolchange");
    def->category = OptionCategory::firmware;
    def->invalidates_step = psGCodeExport;
    def->tooltip = L("This setting allows you to modify the time estimation by a flat amount for each toolchange.");
    def->mode = comExpert | comSuSi;
    def->sidetext = L("s");
    def->min = 0;
    def->set_default_value(new ConfigOptionFloat(30));

    def = definition.add("toolchange_gcode", coString, ptFFF);
    def->label = L("Tool change G-code");
    def->category = OptionCategory::customgcode;
    def->invalidates_step = psGCodeExport;
    def->tooltip = L("This custom code is inserted at every extruder change. If you don't leave this empty, you are "
        "expected to take care of the toolchange yourself - Slic3r will not output any other G-code to "
        "change the filament. You can use placeholder variables for all Slic3r settings as well as {toolchange_z}, {layer_z}, {layer_num}, {max_layer_z}, {previous_extruder} "
        "and {next_extruder}, so e.g. the standard toolchange command can be scripted as T{next_extruder}."
        "!! Warning !!: if any character is written here, Slic3r won't output any toolchange command by itself.");
    def->multiline = true;
    def->full_width = true;
    def->height = 5;
    def->mode = comExpert | comPrusa;
    def->set_default_value(new ConfigOptionString(""));

    def = definition.add("tool_name", coStrings, ptFFF);
    def->label = L("Tool name");
    def->category = OptionCategory::extruders;
    def->invalidates_step = psGCodeExport;
    def->tooltip = L("Only used for Klipper, where you can name the extruder. If not set, will be 'extruderX' with 'X' replaced by the extruder number.");
    def->mode = comExpert | comSuSi;
    def->is_vector_extruder = true;
    def->set_default_value(new ConfigOptionStrings({""}));

    def = definition.add("top_fan_speed", coInts, ptFFF);
    def->label = L("Top Solid fan speed");
    def->category = OptionCategory::cooling;
    def->invalidates_step = psGCodeExport;
    def->tooltip = L("This fan speed is enforced during all top fills (including ironing)."
        "\nSet to 0 to stop the fan."
        "\nIf disabled, Solid Infill fan speed will be used."
        "\nCan be disabled by disable_fan_first_layers, slowed down by full_fan_speed_layer.");
    def->sidetext = L("%");
    def->min = 0;
    def->max = 100;
    def->mode = comAdvancedE | comSuSi;
    def->is_vector_extruder = true;
    def->can_be_disabled = true;
    def->set_default_value(disable_default_option(new ConfigOptionInts({ 100 })));

    def = definition.add("top_infill_extrusion_width", coFloatOrPercent, ptFFF);
    def->label = L("Top solid infill");
    def->category = OptionCategory::width;
    def->invalidates_step = posInfill;
    def->tooltip = L("Set this to a non-zero value to set a manual extrusion width for infill for top surfaces. "
        "You may want to use thinner extrudates to fill all narrow regions and get a smoother finish. "
        "If left as zero, default extrusion width will be used if set, otherwise nozzle diameter will be used. "
        "If expressed as percentage (for example 110%) it will be computed over nozzle diameter."
        "\nYou can set either 'Spacing', or 'Width'; the other will be calculated, using default layer height.");
    def->sidetext = L("mm or %");
    def->ratio_over = "nozzle_diameter";
    def->min = 0;
    def->max = 1000;
    def->max_literal = { 10, true };
    def->precision = 6;
    def->can_phony = true;
    def->mode = comAdvancedE | comPrusa;
    def->set_default_value(new ConfigOptionFloatOrPercent(105, true));

    def = definition.add("top_infill_extrusion_spacing", coFloatOrPercent, ptFFF);
    def->label = L("Top solid spacing");
    def->category = OptionCategory::width;
    def->invalidates_step = posInfill;
    def->tooltip = L("Like Top solid infill width but spacing is the distance between two lines (as they overlap a bit, it's not the same)."
        "\nYou can set either 'Spacing', or 'Width'; the other will be calculated, using default layer height.");
    def->sidetext = L("mm or %");
    def->ratio_over = "nozzle_diameter";
    def->min = 0;
    def->max = 1000;
    def->max_literal = { 10, true };
    def->precision = 6;
    def->can_phony = true;
    def->mode = comAdvancedE | comSuSi;
    def->set_default_value((new ConfigOptionFloatOrPercent(0, false))->set_phony(true));

    def = definition.add("top_solid_infill_acceleration", coFloatOrPercent, ptFFF);
    def->label = L("Top solid ");
    def->full_label = L("Top solid acceleration");
    def->category = OptionCategory::speed;
    def->invalidates_step = psGCodeExport;
    def->tooltip = L("This is the acceleration your printer will use for top solid infill. "
                "\nCan be a % of the solid infill acceleration"
                "\nSet zero or 100% to use solid infill acceleration for top solid infill.");
    def->sidetext = L("mm/s² or %");
    def->ratio_over = "solid_infill_acceleration";
    def->min = 0;
    def->max_literal = { -200, false };
    def->mode = comExpert | comPrusa;
    def->set_default_value(new ConfigOptionFloatOrPercent(0,false));

    def = definition.add("top_solid_infill_overlap", coPercent, ptFFF);
    def->label = L("Top solid infill overlap");
    def->category = OptionCategory::width;
    def->invalidates_step = posPerimeters;
    def->tooltip = L("This setting allows you to reduce the overlap between the lines of the top solid fill, to reduce the % filled if you see overextrusion signs on solid areas."
        "\nNote that you should be sure that your flow (filament extrusion multiplier) is well calibrated and your filament max overlap is set before thinking to modify this."
        "\nAlso, lowering it below 100% may create visible gaps in the top surfaces"
        "\nSet overlap setting is the only one that can't be reduced by the filament's max overlap.");
    def->sidetext = L("%");
    def->min = 0;
    def->max = 100;
    def->mode = comExpert | comSuSi;
    def->set_default_value(new ConfigOptionPercent(100));

    def = definition.add("top_solid_infill_speed", coFloatOrPercent, ptFFF);
    def->label = L("Top solid");
    def->full_label = L("Top solid speed");
    def->category = OptionCategory::speed;
    def->invalidates_step = psGCodeExport;
    def->tooltip = L("Speed for printing top solid layers (it only applies to the uppermost "
                   "external layers and not to their internal solid layers). You may want "
                   "to slow down this to get a nicer surface finish."
                   "\nThis can be expressed as a percentage (for example: 80%) over the Solid Infill speed."
                   "\nSet zero to use autospeed for this feature.");
    def->sidetext = L("mm/s or %");
    def->ratio_over = "solid_infill_speed";
    def->min = 0;
    def->mode = comExpert | comPrusa;
    def->set_default_value(new ConfigOptionFloatOrPercent(50, true));

    def = definition.add("top_solid_layers", coInt, ptFFF);
    //TRN Print Settings: "Top solid layers"
    def->label = L_CONTEXT("Top", "Layers");
    def->full_label = L("Top layers");
    def->category = OptionCategory::perimeter;
    def->invalidates_step = posPrepareInfill;
    def->tooltip = L("Number of solid layers to generate on top surfaces.");
    def->full_label = L("Top solid layers");
    def->min = 0;
    def->mode = comSimpleAE | comPrusa;
    def->set_default_value(new ConfigOptionInt(3));

    def = definition.add("top_solid_min_thickness", coFloat, ptFFF);
    def->label = L_CONTEXT("Top", "Layers");
    def->category = OptionCategory::perimeter;
    def->invalidates_step = posPrepareInfill;
    def->tooltip = L("The number of top solid layers is increased above top_solid_layers if necessary to satisfy "
                     "minimum thickness of top shell."
                     " This is useful to prevent pillowing effect when printing with variable layer height.");
    def->full_label = L("Minimum top shell thickness");
    def->sidetext = L("mm");
    def->min = 0;
    def->mode = comSimpleAE | comPrusa;
    def->set_default_value(new ConfigOptionFloat(0.));

    def = definition.add("travel_acceleration", coFloatOrPercent, ptFFF);
    def->label = L("Travel");
    def->full_label = L("Travel acceleration");
    def->category = OptionCategory::speed;
    def->invalidates_step = psGCodeExport;
    def->tooltip = L("Acceleration for travel moves (jumps between distant extrusion points)."
                     "\nCan be a % of the default acceleration"
                     "\nSet zero to use default acceleration for travel moves.");
    def->sidetext = L("mm/s² or %");
    def->ratio_over = "default_acceleration";
    def->min = 0;
    def->max_literal = { -200, false };
    def->mode = comAdvancedE | comSuSi;
    def->set_default_value(new ConfigOptionFloatOrPercent(1500, false));

    def = definition.add("travel_deceleration_use_target", coBool, ptFFF);
    def->label = L("Decelerate with target acceleration");
    def->full_label = L("Use target acceleration for travel deceleration");
    def->category = OptionCategory::speed;
    def->invalidates_step = psGCodeExport;
    def->tooltip = L("If selected, the deceleration of a travel will use the acceleration value of the extrusion that will be printed after it (if any) ");
    def->mode = comExpert | comSuSi;
    def->set_default_value(new ConfigOptionBool(true));

    def = definition.add("travel_speed", coFloat, ptFFF);
    def->label = L("Travel");
    def->full_label = L("Travel speed");
    def->category = OptionCategory::speed;
    def->invalidates_step = psSkirtBrim;
    def->tooltip = L("Speed for travel moves (jumps between distant extrusion points).");
    def->sidetext = L("mm/s");
    def->aliases = { "travel_feed_rate" };
    def->min = 1;
    def->mode = comAdvancedE | comPrusa;
    def->set_default_value(new ConfigOptionFloat(130));

    def = definition.add("travel_speed_z", coFloat, ptFFF);
    def->label = L("Z Travel");
    def->full_label = L("Z travel speed");
    def->category = OptionCategory::speed;
    def->invalidates_step = psSkirtBrim;
    def->tooltip = L("Speed for movements along the Z axis.\nWhen set to zero, the value "
                     "is ignored and regular travel speed is used instead.");
    def->sidetext = L("mm/s");
    def->aliases = { "travel_feed_rate_z" };
    def->min = 0;
    def->mode = comExpert | comPrusa;
    def->set_default_value(new ConfigOptionFloat(0.));

    def = definition.add("use_firmware_retraction", coBool, ptFFF);
    def->label = L("Use firmware retraction");
    def->category = OptionCategory::general;
    def->invalidates_step = psGCodeExport;
    def->tooltip = L("This setting uses G10 and G11 commands to have the firmware "
                   "handle the retraction. Note that this has to be supported by firmware.");
    def->mode = comExpert | comPrusa;
    def->set_default_value(new ConfigOptionBool(false));

    def = definition.add("use_relative_e_distances", coBool, ptFFF);
    def->label = L("Use relative E distances");
    def->category = OptionCategory::general;
    def->invalidates_step = psGCodeExport;
    def->tooltip = L("If your firmware requires relative E values, check this, "
                   "otherwise leave it unchecked. Most firmwares use absolute values.");
    def->mode = comExpert | comPrusa;
    def->set_default_value(new ConfigOptionBool(false));

    def = definition.add("use_volumetric_e", coBool, ptFFF);
    def->label = L("Use volumetric E");
    def->category = OptionCategory::general;
    def->invalidates_step = psGCodeExport;
    def->tooltip = L("This experimental setting uses outputs the E values in cubic millimeters "
                   "instead of linear millimeters. If your firmware doesn't already know "
                   "filament diameter(s), you can put commands like 'M200 D{filament_diameter_0} T0' "
                   "in your start G-code in order to turn volumetric mode on and use the filament "
                   "diameter associated to the filament selected in Slic3r. This is only supported "
                   "in recent Marlin.");
    def->mode = comExpert | comPrusa;
    def->set_default_value(new ConfigOptionBool(false));

    def = definition.add("variable_layer_height", coBool, ptFFF);
    def->label = L("Enable variable layer height feature");
    def->category = OptionCategory::general;
    def->invalidates_step = psGCodeExport;
    def->tooltip = L("Some printers or printer setups may have difficulties printing "
                   "with a variable layer height. Enabled by default.");
    def->mode = comExpert | comPrusa;
    def->set_default_value(new ConfigOptionBool(true));

    def = definition.add("wipe", coBools, ptFFF);
    def->label = L("Wipe while retracting");
    def->category = OptionCategory::extruders;
    def->invalidates_step = psGCodeExport;
    def->tooltip = L("This flag will move the nozzle while retracting to minimize the possible blob on leaky extruders."
        "\nNote that as a wipe only happens when there is a retraction, the 'only retract when crossing perimeters' print setting can greatly reduce the number of wipes.");
    def->mode = comAdvancedE | comPrusa;
    def->is_vector_extruder = true;
    def->set_default_value(new ConfigOptionBools{ false });

    def = definition.add("wipe_extra_perimeter", coFloats, ptFFF);
    def->category = OptionCategory::extruders;
    def->invalidates_step = psGCodeExport;
    def->label = L("Extra Wipe for external perimeters");
    def->tooltip = L("When the external perimeter loop extrusion ends, a wipe is done, going slightly inside the print."
        " The number in this settting increases the wipe by moving the nozzle along the loop again before the final wipe.");
    def->min = 0;
    def->sidetext = L("mm");
    def->mode = comAdvancedE | comSuSi;
    def->is_vector_extruder = true;
    def->set_default_value(new ConfigOptionFloats{ 0.f });

    def = definition.add("wipe_inside_start", coBools, ptFFF);
    def->label = L("Wipe inside at start");
    def->category = OptionCategory::extruders;
    def->invalidates_step = psGCodeExport;
    def->tooltip = L("Before extruding an external perimeter, this flag will place the nozzle a bit inward and in advance of the seam position before unretracting."
        " It will then move to the seam position before extruding.");
    def->mode = comAdvancedE | comSuSi;
    def->is_vector_extruder = true;
    def->set_default_value(new ConfigOptionBools{ false });

    def = definition.add("wipe_inside_end", coBools, ptFFF);
    def->label = L("Wipe inside at end");
    def->category = OptionCategory::extruders;
    def->invalidates_step = psGCodeExport;
    def->tooltip = L("This flag will wipe the nozzle a bit inward after extruding an external perimeter."
        " The wipe_extra_perimeter is executed first, then this move inward before the retraction wipe."
        " Note that the retraction wipe will follow the exact external perimeter (center) line if this parameter is disabled, and will follow the inner side of the external perimeter line if enabled");
    def->mode = comAdvancedE | comSuSi;
    def->is_vector_extruder = true;
    def->set_default_value(new ConfigOptionBools{ true });

    def = definition.add("wipe_inside_depth", coPercents, ptFFF);
    def->label = L("Max Wipe deviation");
    def->full_label = L("Maximum Wipe deviation to the inside");
    def->category = OptionCategory::extruders;
    def->invalidates_step = psGCodeExport;
    def->tooltip = L("By how much the 'wipe inside' can dive inside the object (if possible)?"
        "\nIn % of the perimeter width."
        "\nNote: don't put a value higher than 50% if you have only one perimeter, or 150% for two perimeter, etc... or it will ooze instead of wipe.");
    def->sidetext = L("%");
    def->mode = comAdvancedE | comSuSi;
    def->is_vector_extruder = true;
    def->set_default_value(new ConfigOptionPercents{ 50 });
    
    def = definition.add("wipe_lift", coFloatsOrPercents, ptFFF);
    def->label = L("Wipe lift");
    def->category = OptionCategory::extruders;
    def->invalidates_step = psGCodeExport;
    def->tooltip = L("When wiping, it will lift gradually to this height, so the filament can be 'cut' more easily."
        "\nCan be a percentage of the current layer height.");
    def->mode = comAdvancedE | comSuSi;
    def->is_vector_extruder = true;
    def->set_default_value(new ConfigOptionFloatsOrPercents{FloatOrPercent{0, false}});
    
    def = definition.add("wipe_lift_length", coFloatsOrPercents, ptFFF);
    def->label = L("Wipe lift length");
    def->full_label = L("Wipe length with lift");
    def->category = OptionCategory::extruders;
    def->invalidates_step = psGCodeExport;
    def->tooltip = L("Distance in the wipe that is used to lift."
        " If higher than the wipe distance, then the lift began at the start of the wipe."
        " If lower than the wipe distance, then the lift began after the start, so the end of the lift occur at the end of the wipe."
        "\nCan be a percentage of the wipe distance.");
    def->mode = comAdvancedE | comSuSi;
    def->is_vector_extruder = true;
    def->set_default_value(new ConfigOptionFloatsOrPercents{FloatOrPercent{50, true}});

    def = definition.add("wipe_min", coFloatsOrPercents, ptFFF);
    def->label = L("Minimum Wipe length");
    def->category = OptionCategory::extruders;
    def->invalidates_step = psGCodeExport;
    def->tooltip = L("Ensure the nozzle will move at least this much."
        "\nCan be a percentage of the needed travel for the retraction"
        " (if this is set to 0, then it's possible that the end of the retraction occur after the end of the wipe).");
    def->mode = comAdvancedE | comSuSi;
    def->is_vector_extruder = true;
    def->set_default_value(new ConfigOptionFloatsOrPercents{FloatOrPercent{150, true}});

    def = definition.add("wipe_only_crossing", coBools, ptFFF);
    def->label = L("Wipe only when crossing perimeters");
    def->category = OptionCategory::extruders;
    def->invalidates_step = psGCodeExport;
    def->tooltip = L("Don't wipe when you don't cross a perimeter. Need 'avoid_crossing_perimeters' and 'wipe' enabled.");
    def->mode = comAdvancedE | comSuSi;
    def->is_vector_extruder = true;
    def->set_default_value(new ConfigOptionBools{ true });

    def = definition.add("wipe_return", coBools, ptFFF);
    def->label = L("return to seam to end the wipe");
    def->category = OptionCategory::extruders;
    def->invalidates_step = psGCodeExport;
    def->tooltip = L("If true, it ensure the wipe ends at the seam. It can stop and return back at mid-distance."
        " If it's a loop, it can continue a bit more or stop early to stop at the right point.");
    def->mode = comExpert | comSuSi;
    def->is_vector_extruder = true;
    def->set_default_value(new ConfigOptionBools{ false });

    def = definition.add("wipe_speed", coFloats, ptFFF);
    def->label = L("Wipe speed");
    def->category = OptionCategory::extruders;
    def->invalidates_step = psGCodeExport;
    def->tooltip = L("Speed in mm/s of the wipe. If it's faster, it will try to go further away, as the wipe time is set by ( 100% - 'retract before wipe') * 'retraction length' / 'retraction speed'."
        "\nIf set to zero, the travel speed is used.");
    def->mode = comAdvancedE | comSuSi;
    def->is_vector_extruder = true;
    def->set_default_value(new ConfigOptionFloats{ 0 });

    def = definition.add("wipe_tower", coBool, ptFFF);
    def->label = L("Enable");
    def->full_label = L("Enable wipe tower");
    def->category = OptionCategory::general;
    def->invalidates_step = psSkirtBrim;
    def->tooltip = L("Multi material printers may need to prime or purge extruders on tool changes. "
                   "Extrude the excess material into the wipe tower.");
    def->mode = comAdvancedE | comPrusa;
    def->set_default_value(new ConfigOptionBool(false));

    def = definition.add("wipe_tower_speed", coFloat, ptFFF);
    def->label = L("Wipe Tower Speed");
    def->category = OptionCategory::speed;
    def->invalidates_step = psSkirtBrim;
    def->tooltip = L("Printing speed of the wipe tower. Capped by filament_max_volumetric_speed (if set)."
        "\nIf set to zero, a value of 80mm/s is used.");
    def->sidetext = L("mm/s");
    def->mode = comAdvancedE | comSuSi;
    def->set_default_value(new ConfigOptionFloat(80.));

    def = definition.add("wipe_tower_wipe_starting_speed", coFloatOrPercent, ptFFF);
    def->label = L("Wipe tower wipe starting speed");
    def->category = OptionCategory::speed;
    def->invalidates_step = psSkirtBrim;
    def->tooltip = L("Start of the wiping speed ramp up (for wipe tower)."
        "\nCan be a % of the 'Wipe tower main speed'."
        "\nSet to 0 to disable.");
    def->sidetext = L("mm/s or %");
    def->mode = comAdvancedE | comSuSi;
    def->set_default_value(new ConfigOptionFloatOrPercent(33, true));

    def = definition.add("wipe_tower_extrusion_width", coFloatOrPercent, ptFFF);
    def->label = L("Wipe Tower purge line width");
    def->category = OptionCategory::width;
    def->invalidates_step = psSkirtBrim;
    def->tooltip = L("When wiping, the extrusion should be at least 125% of the nozzle diameter."
        " This setting allow you to vary it, in case you need a wider one to properly flush the nozzle.");
    def->sidetext = L("mm or %");
    def->mode = comAdvancedE | comSuSi;
    def->set_default_value(new ConfigOptionFloatOrPercent(150, true));

    def = definition.add("wiping_volumes_extruders", coFloats, ptFFF);
    def->label = L("Purging volumes - load/unload volumes");
    def->invalidates_step = psSkirtBrim;
    def->tooltip = L("This vector saves required volumes to change from/to each tool used on the "
                     "wipe tower. These values are used to simplify creation of the full purging "
                     "volumes below. ");
    def->mode = comSimpleAE | comPrusa;
    def->set_default_value(new ConfigOptionFloats { 70., 70., 70., 70., 70., 70., 70., 70., 70., 70.  });

    def = definition.add("wiping_volumes_matrix", coFloats, ptFFF);
    def->label = L("Purging volumes - matrix");
    def->invalidates_step = psSkirtBrim;
    def->tooltip = L("This matrix describes volumes (in cubic milimetres) required to purge the"
                     " new filament on the wipe tower for any given pair of tools. ");
    def->mode = comSimpleAE | comPrusa;
    def->set_default_value(new ConfigOptionFloats {   0., 140., 140., 140., 140.,
                                                    140.,   0., 140., 140., 140.,
                                                    140., 140.,   0., 140., 140.,
                                                    140., 140., 140.,   0., 140.,
                                                    140., 140., 140., 140.,   0. });


    def = definition.add("wipe_advanced", coBool, ptFFF);
    def->label = L("Enable advanced wiping volume");
    def->invalidates_step = psGCodeExport;
    def->tooltip = L("Allow Slic3r to compute the purge volume via smart computations. Use the pigment% of each filament and following parameters");
    def->mode = comExpert | comSuSi;
    def->set_default_value(new ConfigOptionBool(false));

    def = definition.add("wipe_advanced_nozzle_melted_volume", coFloat, ptFFF);
    def->label = L("Nozzle volume");
    def->invalidates_step = psGCodeExport;
    def->tooltip = L("The volume of melted plastic inside your nozzle. Used by 'advanced wiping'.");
    def->sidetext = L("mm3");
    def->mode = comExpert | comSuSi;
    def->set_default_value(new ConfigOptionFloat(120));

    def = definition.add("filament_wipe_advanced_pigment", coFloats, ptFFF);
    def->label = L("Pigment percentage");
    def->invalidates_step = psGCodeExport;
    def->tooltip = L("The pigment % for this filament (between 0 and 1, 1=100%). 0 for translucent/natural, 0.2-0.5 for white and 1 for black.");
    def->min = 0;
    def->max = 1;
    def->mode = comExpert | comSuSi;
    def->is_vector_extruder = true;
    def->set_default_value(new ConfigOptionFloats{ 0.5 });

    def = definition.add("wipe_advanced_multiplier", coFloat, ptFFF);
    def->label = L("Multiplier");
    def->full_label = L("Auto-wipe multiplier");
    def->invalidates_step = psGCodeExport;
    def->tooltip = L("The volume multiplier used to compute the final volume to extrude by the algorithm.");
    def->sidetext = L("mm3");
    def->mode = comExpert | comSuSi;
    def->set_default_value(new ConfigOptionFloat(60));


    def = definition.add("wipe_advanced_algo", coEnum, ptFFF);
    def->label = L("Algorithm");
    def->full_label = L("Auto-wipe algorithm");
    def->invalidates_step = psGCodeExport;
    def->tooltip = L("Algorithm for the advanced wipe.\n"
        "Linear : volume = nozzle + volume_mult * (pigmentBefore-pigmentAfter)\n"
        "Quadratic: volume = nozzle + volume_mult * (pigmentBefore-pigmentAfter)+ volume_mult * (pigmentBefore-pigmentAfter)^3\n"
        "Hyperbola: volume = nozzle + volume_mult * (0.5+pigmentBefore) / (0.5+pigmentAfter)");
    def->set_enum<WipeAlgo>({
        { "linear", L("Linear") }, 
        { "quadra", L("Quadratric") },
        { "expo",   L("Hyperbola") }
    });
    def->mode = comExpert | comSuSi;
    def->set_default_value(new ConfigOptionEnum<WipeAlgo>(waLinear));

    def = definition.add("wipe_tower_brim_width", coFloatOrPercent, ptFFF);
    def->label = L("Wipe tower brim width");
    def->invalidates_step = psSkirtBrim;
    def->tooltip = L("Width of the brim for the wipe tower. Can be in mm or in % of the (assumed) only one nozzle diameter.");
    def->ratio_over = "nozzle_diameter";
    def->mode = comAdvancedE | comPrusa;
    def->min = 0;
    def->max_literal = { 100, true };
    def->aliases = { "wipe_tower_brim" }; // SuperSlicer 2.3 and before
    def->set_default_value(new ConfigOptionFloatOrPercent(2,false));

    def = definition.add("wipe_tower_no_sparse_layers", coBool, ptFFF);
    def->label = L("No sparse layers (EXPERIMENTAL)");
    def->category = OptionCategory::mmsetup;
    def->invalidates_step = psSkirtBrim;
    def->tooltip = L("If enabled, the wipe tower will not be printed on layers with no toolchanges. "
                     "On layers with a toolchange, extruder will travel downward to print the wipe tower. "
                     "User is responsible for ensuring there is no collision with the print.");
    def->mode = comAdvancedE | comPrusa;
    def->set_default_value(new ConfigOptionBool(false));

    def = definition.add("wipe_tower_rest_in_middle", coBool, ptFFF);
    def->label = L("toolchange inside the wipe tower");
    def->category = OptionCategory::mmsetup;
    def->invalidates_step = psSkirtBrim;
    def->tooltip = L("If enabled, there will be a travel inside the wipe tower before the toolchange.");
    def->mode = comAdvancedE | comSuSi;
    def->set_default_value(new ConfigOptionBool(false));

    //TODO: combine x&y into a coPoint
    def = definition.add("wipe_tower_x", coFloat, ptFFF);
    def->label = L("X");
    def->full_label = L("Wipe tower X");
    def->invalidates_step = psSkirtBrim;
    def->tooltip = L("X coordinate of the left front corner of a wipe tower");
    def->sidetext = L("mm");
    def->mode = comAdvancedE | comPrusa;
    def->set_default_value(new ConfigOptionFloat(180.));


    def = definition.add("wipe_tower_y", coFloat, ptFFF);
    def->label = L("Y");
    def->full_label = L("Wipe tower Y");
    def->invalidates_step = psSkirtBrim;
    def->tooltip = L("Y coordinate of the left front corner of a wipe tower");
    def->sidetext = L("mm");
    def->mode = comAdvancedE | comPrusa;
    def->set_default_value(new ConfigOptionFloat(140.));

    def = definition.add("wipe_tower_width", coFloat, ptFFF);
    def->label = L("Width");
    def->full_label = L("Wipe tower Width");
    def->invalidates_step = psSkirtBrim;
    def->tooltip = L("Width of a wipe tower");
    def->sidetext = L("mm");
    def->mode = comAdvancedE | comPrusa;
    def->set_default_value(new ConfigOptionFloat(60.));

    def = definition.add("wipe_tower_rotation_angle", coFloat, ptFFF);
    def->label = L("Wipe tower rotation angle");
    def->invalidates_step = psSkirtBrim;
    def->tooltip = L("Wipe tower rotation angle with respect to x-axis.");
    def->sidetext = L("°");
    def->mode = comAdvancedE | comPrusa;
    def->set_default_value(new ConfigOptionFloat(0.));

    def = definition.add("wipe_tower_cone_angle", coFloat, ptFFF);
    def->label = L("Stabilization cone apex angle");
    def->invalidates_step = psSkirtBrim;
    def->tooltip = L("Angle at the apex of the cone that is used to stabilize the wipe tower. "
                     "Larger angle means wider base.");
    def->sidetext = L("°");
    def->mode = comAdvancedE |comPrusa;
    def->min = 0.;
    def->max = 90.;
    def->set_default_value(new ConfigOptionFloat(0.));

    def = definition.add("wipe_tower_extra_spacing", coPercent, ptFFF);
    def->label = L("Wipe tower purge lines spacing");
    def->invalidates_step = psSkirtBrim;
    def->tooltip = L("Spacing of purge lines on the wipe tower.");
    def->sidetext = L("%");
    def->mode = comExpert |comPrusa;
    def->min = 100.;
    def->max = 300.;
    def->set_default_value(new ConfigOptionPercent(100.));

    def = definition.add("wipe_into_infill", coBool, ptFFF);
    def->category = OptionCategory::wipe;
    def->invalidates_step = psWipeTower;
    def->label = L("Wipe into this object's infill");
    def->tooltip = L("Purging after toolchange will be done inside this object's infills. "
                     "This lowers the amount of waste but may result in longer print time "
                     " due to additional travel moves.");
    def->mode = comSimpleAE | comPrusa;
    def->set_default_value(new ConfigOptionBool(false));

    def = definition.add("wipe_into_objects", coBool, ptFFF);
    def->category = OptionCategory::wipe;
    def->invalidates_step = psWipeTower;
    def->label = L("Wipe into this object");
    def->tooltip = L("Object will be used to purge the nozzle after a toolchange to save material "
        "that would otherwise end up in the wipe tower and decrease print time. "
        "Colours of the objects will be mixed as a result.");
    def->mode = comSimpleAE | comPrusa;
    def->set_default_value(new ConfigOptionBool(false));

    def = definition.add("wipe_tower_bridging", coFloat, ptFFF);
    def->label = L("Maximal bridging distance");
    def->invalidates_step = psSkirtBrim;
    def->tooltip = L("Maximal distance between supports on sparse infill sections. ");
    def->sidetext = L("mm");
    def->mode = comAdvancedE | comPrusa;
    def->set_default_value(new ConfigOptionFloat(10.));

    def = definition.add("wipe_tower_extruder", coInt, ptFFF);
    def->label = L("Wipe tower extruder");
    def->category = OptionCategory::extruders;
    def->invalidates_step = psSkirtBrim;
    def->tooltip = L("The extruder to use when printing perimeter of the wipe tower. "
                     "Set to 0 to use the one that is available (non-soluble would be preferred).");
    def->min = 0;
    def->mode = comAdvancedE |comPrusa;
    def->set_default_value(new ConfigOptionInt(0));

    def = definition.add("solid_infill_every_layers", coInt, ptFFF);
    def->label = L("Solid infill every");
    def->category = OptionCategory::infill;
    def->invalidates_step = posPrepareInfill;
    def->tooltip = L("This feature allows to force a solid layer every given number of layers. "
                   "Zero to disable. You can set this to any value (for example 9999); "
                   "Slic3r will automatically choose the maximum possible number of layers "
                   "to combine according to nozzle diameter and layer height.");
    def->sidetext = L("layers");
    def->min = 0;
    def->mode = comExpert |comPrusa;
    def->set_default_value(new ConfigOptionInt(0));

    def = definition.add("xy_size_compensation", coFloat, ptFFF);
    def->label = L("Outer");
    def->full_label = L("Outer XY size compensation");
    def->category = OptionCategory::slicing;
    def->invalidates_step = posSlice;
    def->tooltip = L("The object will be grown/shrunk in the XY plane by the configured value "
        "(negative = inwards = remove area, positive = outwards = add area). This might be useful for fine-tuning sizes."
        "\nThis one only applies to the 'exterior' shell of the object."
        "\n !!! it's recommended you put the same value into the 'Inner XY size compensation', unless you are sure you don't have horizontal holes. !!! ");
    def->sidetext = L("mm");
    def->mode = comExpert | comSuSi;
    def->set_default_value(new ConfigOptionFloat(0));

    def = definition.add("xy_inner_size_compensation", coFloat, ptFFF);
    def->label = L("Inner");
    def->full_label = L("Inner XY size compensation");
    def->category = OptionCategory::slicing;
    def->invalidates_step = posSlice;
    def->tooltip = L("The object will be grown/shrunk in the XY plane by the configured value "
        "(negative = inwards = remove area, positive = outwards = add area). This might be useful for fine-tuning sizes."
        "\nThis one only applies to the 'inner' shell of the object (!!! horizontal holes break the shell !!!)");
    def->sidetext = L("mm");
    def->mode = comExpert | comSuSi;
    def->set_default_value(new ConfigOptionFloat(0));

    def = definition.add("hole_size_compensation", coFloat, ptFFF);
    def->label = L("XY compensation");
    def->full_label = L("XY holes compensation");
    def->category = OptionCategory::slicing;
    def->invalidates_step = posSlice;
    def->tooltip = L("The convex holes will be grown / shrunk in the XY plane by the configured value"
        " (negative = inwards = remove area, positive = outwards = add area, should be negative as the holes are always a bit smaller irl)."
        " This might be useful for fine-tuning hole sizes."
        "\nThis setting behaves the same as 'Inner XY size compensation' but only for convex shapes. It's added to 'Inner XY size compensation', it does not replace it. ");
    def->sidetext = L("mm");
    def->mode = comExpert | comSuSi;
    def->set_default_value(new ConfigOptionFloat(0));

    def = definition.add("hole_size_threshold", coFloat, ptFFF);
    def->label = L("Threshold");
    def->full_label = L("XY holes threshold");
    def->category = OptionCategory::slicing;
    def->invalidates_step = posSlice;
    def->tooltip = L("Maximum area for the hole where the hole_size_compensation will apply fully."
            " After that, it will decrease down to 0 for four times this area."
            " Set to 0 to let the hole_size_compensation apply fully for all detected holes");
    def->sidetext = L("mm²");
    def->mode = comExpert | comSuSi;
    def->set_default_value(new ConfigOptionFloat(100));

    def = definition.add("z_offset", coFloat, ptFFF);
    def->label = L("Z offset");
    def->category = OptionCategory::general;
    def->invalidates_step = psSkirtBrim;
    def->tooltip = L("This value will be added (or subtracted) from all the Z coordinates "
                   "in the output G-code. It is used to compensate for bad Z endstop position: "
                   "for example, if your endstop zero actually leaves the nozzle 0.3mm far "
                   "from the print bed, set this to -0.3 (or fix your endstop).");
    def->sidetext = L("mm");
    def->mode = comExpert | comPrusa;
    def->set_default_value(new ConfigOptionFloat(0));

    def = definition.add("z_step", coFloat, ptFFF);
    def->label = L("Z full step");
    def->invalidates_step = posSlice;
    def->tooltip = L("Set this to the height moved when your Z motor (or equivalent) turns one step."
                    "If your motor needs 200 steps to move your head/platter by 1mm, this field should be 1/200 = 0.005."
                    "\nNote that the gcode will write the z values with 6 digits after the dot if z_step is set (it's 3 digits if it's disabled)."
                    "\nSet zero to disable.");
    def->cli = "z-step=f";
    def->sidetext = L("mm");
    def->min = 0;
    def->precision = 8;
    def->mode = comExpert | comSuSi;
    def->set_default_value(new ConfigOptionFloat(0.0));

    def = definition.add("init_z_rotate", coFloat, ptFFF);
    def->label = L("Preferred orientation");
    def->category = OptionCategory::general;
    def->invalidates_step = STEP_NONE;
    def->tooltip = L("Rotate stl around z axes while adding them to the bed.");
    def->sidetext = L("°");
    def->min = -360;
    def->max = 360;
    def->mode = comAdvancedE | comSuSi;
    def->set_default_value(new ConfigOptionFloat(0.0));

    //// Arachne slicing, put it in alpha order when out of experimental

    def = definition.add("perimeter_generator", coEnum, ptFFF);
    def->label = L("Perimeter generator");
    def->category = OptionCategory::perimeter;
    def->invalidates_step = posSlice;
    def->tooltip = L("Classic perimeter generator produces perimeters with constant extrusion width and for "
                      "very thin areas is used gap-fill. "
                      "Arachne engine produces perimeters with variable extrusion width. "
                      "This setting also affects the Concentric infill.");
    def->set_enum<PerimeterGeneratorType>({
        { "classic", L("Classic") },
        { "arachne", L("Arachne") }
    });
    def->mode = comAdvancedE | comPrusa;
    def->set_default_value(new ConfigOptionEnum<PerimeterGeneratorType>(PerimeterGeneratorType::Classic));

    def = definition.add("wall_transition_length", coFloatOrPercent, ptFFF);
    def->label = L("Perimeter transition length");
    def->category = OptionCategory::advanced;
    def->invalidates_step = posSlice;
    def->tooltip  = L("When transitioning between different numbers of perimeters as the part becomes"
                       "thinner, a certain amount of space is allotted to split or join the perimeter segments. "
                       "If expressed as a percentage (for example 100%), it will be computed based on the nozzle diameter.");
    def->sidetext = L("mm or %");
    def->mode = comExpert | comPrusa;
    def->min = 0;
    def->set_default_value(new ConfigOptionFloatOrPercent(100, true));

    def = definition.add("wall_transition_filter_deviation", coFloatOrPercent, ptFFF);
    def->label = L("Perimeter transitioning filter margin");
    def->category = OptionCategory::advanced;
    def->invalidates_step = posSlice;
    def->tooltip  = L("Prevent transitioning back and forth between one extra perimeter and one less. This "
                       "margin extends the range of extrusion widths which follow to [Minimum perimeter width "
                       "- margin, 2 * Minimum perimeter width + margin]. Increasing this margin "
                       "reduces the number of transitions, which reduces the number of extrusion "
                       "starts/stops and travel time. However, large extrusion width variation can lead to "
                       "under- or overextrusion problems."
                       "If expressed as percentage (for example 25%), it will be computed over nozzle diameter.");
    def->sidetext = L("mm");
    def->mode = comExpert | comPrusa;
    def->min = 0;
    def->set_default_value(new ConfigOptionFloatOrPercent(25, true));

    def = definition.add("wall_transition_angle", coFloat, ptFFF);
    def->label = L("Perimeter transitioning threshold angle");
    def->category = OptionCategory::advanced;
    def->invalidates_step = posSlice;
    def->tooltip  = L("When to create transitions between even and odd numbers of perimeters. A wedge shape with"
                       " an angle greater than this setting will not have transitions and no perimeters will be "
                       "printed in the center to fill the remaining space. Reducing this setting reduces "
                       "the number and length of these center perimeters, but may leave gaps or overextrude.");
    def->sidetext = L("°");
    def->mode = comExpert | comPrusa;
    def->min = 1.;
    def->max = 59.;
    def->set_default_value(new ConfigOptionFloat(10.));

    def = definition.add("wall_distribution_count", coInt, ptFFF);
    def->label = L("Perimeter distribution count");
    def->category = OptionCategory::advanced;
    def->invalidates_step = posSlice;
    def->tooltip  = L("The number of perimeters, counted from the center, over which the variation needs to be "
                       "spread. Lower values mean that the outer perimeters don't change in width.");
    def->mode = comExpert | comPrusa;
    def->min = 1;
    def->set_default_value(new ConfigOptionInt(1));

    def = definition.add("min_feature_size", coFloatOrPercent, ptFFF);
    def->label = L("Minimum feature size");
    def->category = OptionCategory::advanced;
    def->invalidates_step = posSlice;
    def->tooltip  = L("Minimum thickness of thin features. Model features that are thinner than this value will "
                       "not be printed, while features thicker than the Minimum feature size will be widened to "
                       "the Minimum perimeter width. "
                       "If expressed as a percentage (for example 25%), it will be computed based on the nozzle diameter.");
    def->sidetext = L("mm or %");
    def->mode = comExpert | comPrusa;
    def->min = 0;
    def->set_default_value(new ConfigOptionFloatOrPercent(25, true));

    def = definition.add("min_bead_width", coFloatOrPercent, ptFFF);
    def->label = L("Minimum perimeter width");
    def->category = OptionCategory::advanced;
    def->invalidates_step = posSlice;
    def->tooltip  = L("Width of the perimeter that will replace thin features (according to the Minimum feature size) "
                       "of the model. If the Minimum perimeter width is thinner than the thickness of the feature,"
                       " the perimeter will become as thick as the feature itself. "
                       "If expressed as percentage (for example 85%), it will be computed over nozzle diameter.");
    def->sidetext = L("mm or %");
    def->mode = comExpert | comPrusa;
    def->min = 0;
    def->set_default_value(new ConfigOptionFloatOrPercent(85, true));

    /////// End of Arachne settings /////

    // Declare retract values for filament profile, overriding the printer's extruder profile.
    for (const std::string &opt_key : definition.option_keys(RAW_PRESET_TYPE_FFF_FILAMENT_OVERRIDE)
        //{
        //// floats
        //"retract_length", "retract_lift", "retract_lift_above", "retract_lift_below", "retract_speed", 
        //"travel_max_lift",
        //"deretract_speed", "retract_restart_extra", "retract_before_travel", "retract_lift_before_travel",
        //"retract_length_toolchange", "retract_restart_extra_toolchange",
        //"wipe_extra_perimeter", "wipe_speed",
        //"wipe_inside_depth", "wipe_inside_end", "wipe_inside_start",
        //// bools
        //"retract_layer_change", "wipe", "wipe_only_crossing", "wipe_return",
        //"travel_lift_before_obstacle", "travel_ramping_lift", "travel_slope",
        //// percents
        //"retract_before_wipe", "travel_slope",
        //// floatsOrPercents
        //"seam_gap"
        //}

        ) {
        auto it_opt = definition.options.find(opt_key);
        assert(it_opt != definition.options.end());
        def = definition.add(std::string("filament_") + opt_key, it_opt->second.type, ptFFF);
        def->can_be_disabled = true;
        def->is_optional = true;
        def->label      = it_opt->second.label;
        def->full_label = it_opt->second.full_label;
        def->tooltip    = it_opt->second.tooltip;
        def->sidetext   = it_opt->second.sidetext;
        def->mode       = it_opt->second.mode;
        // create default value with the default value is taken from the default value of the config.
        // put a disabled value as first entry.
        switch (def->type) {
        case coBools: {
            ConfigOptionBools *opt = new ConfigOptionBools({it_opt->second.default_value.get()->get_bool()});
            opt->set_can_be_disabled(true);
            def->set_default_value(opt);
            break;
        }
        case coFloats: {
            ConfigOptionFloats *opt = new ConfigOptionFloats({it_opt->second.default_value.get()->get_float()});
            opt->set_can_be_disabled(true);
            def->set_default_value(opt);
            break;
        }
        case coPercents: {
            ConfigOptionPercents *opt = new ConfigOptionPercents({it_opt->second.default_value.get()->get_float()});
            opt->set_can_be_disabled(true);
            def->set_default_value(opt);
            break;
        }
        case coFloatsOrPercents: {
            ConfigOptionFloatsOrPercents*opt = new ConfigOptionFloatsOrPercents(
                {static_cast<const ConfigOptionFloatsOrPercents*>(it_opt->second.default_value.get())->get_at(0)});
            opt->set_can_be_disabled(true);
            def->set_default_value(opt);
            break;
        }
        default: assert(false);
        }
        assert(!def->default_value->is_enabled());
    }

}


} // namespace Slic3r
