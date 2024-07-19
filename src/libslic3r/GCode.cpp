///|/ Copyright (c) Prusa Research 2016 - 2023 Lukáš Matěna @lukasmatena, Vojtěch Bubník @bubnikv, Enrico Turri @enricoturri1966, Pavel Mikuš @Godrak, Oleksandra Iushchenko @YuSanka, Lukáš Hejl @hejllukas, Filip Sykala @Jony01, David Kocík @kocikdav
///|/ Copyright (c) SuperSlicer 2023 Remi Durand @supermerill
///|/ Copyright (c) 2021 Justin Schuh @jschuh
///|/ Copyright (c) 2020 Paul Arden @ardenpm
///|/ Copyright (c) 2020 sckunkle
///|/ Copyright (c) 2020 Kyle Maas @KyleMaas
///|/ Copyright (c) 2019 Thomas Moore
///|/ Copyright (c) 2019 Bryan Smith
///|/ Copyright (c) Slic3r 2015 - 2016 Alessandro Ranellucci @alranel
///|/ Copyright (c) 2016 Chow Loong Jin @hyperair
///|/ Copyright (c) 2015 Maksim Derbasov @ntfshard
///|/ Copyright (c) 2015 Vicious-one @Vicious-one
///|/ Copyright (c) 2015 Luís Andrade
///|/
///|/ ported from lib/Slic3r/GCode.pm:
///|/ Copyright (c) Slic3r 2011 - 2015 Alessandro Ranellucci @alranel
///|/ Copyright (c) 2013 Robert Giseburt
///|/ Copyright (c) 2012 Mark Hindess
///|/ Copyright (c) 2012 Henrik Brix Andersen @henrikbrixandersen
///|/
///|/ PrusaSlicer is released under the terms of the AGPLv3 or higher
///|/
#include "Color.hpp"
#include "Config.hpp"
#include "Geometry/Circle.hpp"
#include "libslic3r.h"
#include "GCode/ExtrusionProcessor.hpp"
#include "I18N.hpp"
#include "GCode.hpp"
#include "Exception.hpp"
#include "ExtrusionEntity.hpp"
#include "Geometry/ConvexHull.hpp"
#include "GCode/FanMover.hpp"
#include "GCode/LabelObjects.hpp"
#include "GCode/PrintExtents.hpp"
#include "GCode/Thumbnails.hpp"
#include "GCode/WipeTower.hpp"
#include "GCode/WipeTowerIntegration.hpp"
#include "GCode/Travels.hpp"
#include "Point.hpp"
#include "Polygon.hpp"
#include "PrintConfig.hpp"
#include "ShortestPath.hpp"
#include "PrintConfig.hpp"
#include "Thread.hpp"
#include "Utils.hpp"
#include "ClipperUtils.hpp"
#include "libslic3r.h"
#include "LocalesUtils.hpp"
#include "format.hpp"

#include <algorithm>
#include <cstdlib>
#include <chrono>
#include <map>
#include <math.h>
#include <unordered_set>
#include <optional>
#include <string>
#include <string_view>

#include <boost/algorithm/string.hpp>
#include <boost/algorithm/string/find.hpp>
#include <boost/algorithm/string/regex.hpp>
#include <boost/foreach.hpp>
#include <boost/format.hpp>
#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>

#include <boost/nowide/iostream.hpp>
#include <boost/nowide/cstdio.hpp>
#include <boost/nowide/cstdlib.hpp>

#include "SVG.hpp"

#include <tbb/parallel_for.h>

// Intel redesigned some TBB interface considerably when merging TBB with their oneAPI set of libraries, see GH #7332.
// We are using quite an old TBB 2017 U7. Before we update our build servers, let's use the old API, which is deprecated in up to date TBB.
#if ! defined(TBB_VERSION_MAJOR)
    #include <tbb/version.h>
#endif
#if ! defined(TBB_VERSION_MAJOR)
    static_assert(false, "TBB_VERSION_MAJOR not defined");
#endif
#if TBB_VERSION_MAJOR >= 2021
    #include <tbb/parallel_pipeline.h>
    using slic3r_tbb_filtermode = tbb::filter_mode;
#else
    #include <tbb/pipeline.h>
    using slic3r_tbb_filtermode = tbb::filter;
#endif

using namespace std::literals::string_view_literals;

#if 0
// Enable debugging and asserts, even in the release build.
#define DEBUG
#define _DEBUG
#undef NDEBUG
#endif

#include <assert.h>

namespace Slic3r {

// Only add a newline in case the current G-code does not end with a newline.
    static inline void check_add_eol(std::string& gcode)
    {
        if (!gcode.empty() && gcode.back() != '\n')
            gcode += '\n';
    }
    

    // Return true if tch_prefix is found in custom_gcode
    static bool custom_gcode_changes_tool(const std::string& custom_gcode, const std::string& tch_prefix, unsigned next_extruder)
    {
        bool ok = false;
        size_t from_pos = 0;
        size_t pos = 0;
        while ((pos = custom_gcode.find(tch_prefix, from_pos)) != std::string::npos) {
            if (pos + 1 == custom_gcode.size())
            break;
            from_pos = pos + 1;
            // only whitespace is allowed before the command
            while (--pos < custom_gcode.size() && custom_gcode[pos] != '\n') {
                    if (!std::isspace(custom_gcode[pos]))
                    goto NEXT;
            }
            {
                // we should also check that the extruder changes to what was expected
                std::istringstream ss(custom_gcode.substr(from_pos, std::string::npos));
                unsigned num = 0;
                if (ss >> num)
                    ok = (num == next_extruder);
            }
        NEXT:;
        }
        return ok;
    }

    double get_default_acceleration(PrintConfig& config) {
        double max = 0;
        max = config.machine_max_acceleration_extruding.get_at(0);
        // on 2.3, check for enable/disable if(config.machine_limits_usage)
        if (config.machine_limits_usage <= MachineLimitsUsage::Limits)
            return std::min(config.get_computed_value("default_acceleration"), max);
        else
            return config.get_computed_value("default_acceleration");
    }

    double get_travel_acceleration(PrintConfig& config) {
        double max = 0;
        max = config.machine_max_acceleration_travel.get_at(0);
        // on 2.3, check for enable/disable if(config.machine_limits_usage)
        if (config.machine_limits_usage <= MachineLimitsUsage::Limits)
            return std::min(config.get_computed_value("travel_acceleration"), max);
        else
            return config.get_computed_value("travel_acceleration");
    }

    std::string OozePrevention::pre_toolchange(GCodeGenerator& gcodegen)
    {
        std::string gcode;

        // move to the nearest standby point (erased in2.7)
        //if (!this->standby_points.empty()) {
        //    // get current position in print coordinates
        //    Vec3d writer_pos = gcodegen.writer().get_position();
        //    Point pos = Point::new_scale(writer_pos(0), writer_pos(1));

        //    // find standby point
        //    Point standby_point;
        //    this->standby_points.nearest_point(pos, &standby_point);

        //    /*  We don't call gcodegen.travel_to() because we don't need retraction (it was already
        //        triggered by the caller) nor avoid_crossing_perimeters and also because the coordinates
        //        of the destination point must not be transformed by origin nor current extruder offset.  */
        //        gcode += gcodegen.writer().travel_to_xy(unscale(standby_point), 0.0, "move to standby position");
        //}
        unsigned int extruder_id = gcodegen.writer().tool()->id();
        const ConfigOptionIntsNullable& filament_idle_temp = gcodegen.config().idle_temperature;
        if (filament_idle_temp.is_nil(extruder_id)) {
            // There is no idle temperature defined in filament settings.
            // Use the delta value from print config.
            if (gcodegen.config().standby_temperature_delta.value != 0 && gcodegen.writer().tool_is_extruder() && this->_get_temp(gcodegen) > 0) {
                // we assume that heating is always slower than cooling, so no need to block
                gcode += gcodegen.writer().set_temperature
                (this->_get_temp(gcodegen) + gcodegen.config().standby_temperature_delta.value, false, extruder_id);
                if(gcode.back() == '\n') gcode.pop_back(); // delete \n if possible to insert our comment FIXME: allow set_temperature to get an extra comment
                gcode += " ;cooldown\n"; // this is a marker for GCodeProcessor, so it can supress the commands when needed
            }
        } else {
            // Use the value from filament settings. That one is absolute, not delta.
            gcode += gcodegen.writer().set_temperature(filament_idle_temp.get_at(extruder_id), false, extruder_id);
            if(gcode.back() == '\n') gcode.pop_back(); // delete \n if possible to insert our comment FIXME: allow set_temperature to get an extra comment
            gcode += " ;cooldown\n"; // this is a marker for GCodeProcessor, so it can supress the commands when needed
        }

        return gcode;
    }

    std::string OozePrevention::post_toolchange(GCodeGenerator& gcodegen)
    {
        if (gcodegen.config().standby_temperature_delta.value != 0 && gcodegen.writer().tool_is_extruder()){
            int temp = this->_get_temp(gcodegen);
            if (temp > 0)
                return gcodegen.writer().set_temperature(temp, true, gcodegen.writer().tool()->id());
        }
        return std::string();
    }

    int OozePrevention::_get_temp(const GCodeGenerator& gcodegen) const
    {
        // First layer temperature should be used when on the first layer (obviously) and when
        // "other layers" is set to zero (which means it should not be used).
        if (gcodegen.writer().tool_is_extruder())
            if ((gcodegen.layer() == nullptr || gcodegen.layer()->id() == 0 
                     || gcodegen.config().temperature.get_at(gcodegen.writer().tool()->id()) == 0)
                 && gcodegen.config().first_layer_temperature.get_at(gcodegen.writer().tool()->id()) > 0)
                return gcodegen.config().first_layer_temperature.get_at(gcodegen.writer().tool()->id());
            else
                return gcodegen.config().temperature.get_at(gcodegen.writer().tool()->id());
        else
            return 0;
    }

    //if first layer, ask for a bigger lift for travel to object, to be on the safe side
    static inline void set_extra_lift(const float previous_print_z, const int layer_id, const PrintConfig& print_config, GCodeWriter & writer, int extruder_id) {
        //if first layer, ask for a bigger lift for travel to object, to be on the safe side
        double extra_lift_value = 0;
        if (print_config.lift_min.value > 0) {
            double retract_lift = 0;
            //get the current lift (imo, should be given by the writer... i'm duplicating stuff here)
            if(//(previous_print_z == 0 && print_config.retract_lift_above.get_at(writer.tool()->id()) == 0) ||
                print_config.retract_lift_above.get_at(writer.tool()->id()) <= previous_print_z + EPSILON
                || (layer_id == 0 && print_config.retract_lift_first_layer.get_at(writer.tool()->id())))
                retract_lift = writer.tool()->retract_lift();
            // see if it's positive
            if (previous_print_z + extra_lift_value + retract_lift < print_config.lift_min.value) {
                extra_lift_value = print_config.lift_min.value - previous_print_z - retract_lift;
            }
        }
        if(extra_lift_value > 0)
            writer.set_extra_lift(extra_lift_value);
    }

    const std::vector<std::string> ColorPrintColors::Colors = { "#C0392B", "#E67E22", "#F1C40F", "#27AE60", "#1ABC9C", "#2980B9", "#9B59B6" };

#define EXTRUDER_CONFIG_WITH_DEFAULT(OPT,DEF) (m_writer.tool_is_extruder()?m_config.OPT.get_at(m_writer.tool()->id()):DEF)
#define BOOL_EXTRUDER_CONFIG(OPT) m_writer.tool_is_extruder() && m_config.OPT.get_at(m_writer.tool()->id())

void GCodeGenerator::PlaceholderParserIntegration::reset()
{
    this->failed_templates.clear();
    this->output_config.clear();
    this->opt_position = nullptr;
    this->opt_position_parser = nullptr;
    this->opt_zhop = nullptr;
    this->opt_e_position = nullptr;
    this->opt_e_retracted = nullptr;
    this->opt_e_restart_extra = nullptr;
    this->opt_extruded_volume = nullptr;
    this->opt_extruded_weight = nullptr;
    this->opt_extruded_volume_total = nullptr;
    this->opt_extruded_weight_total = nullptr;
    this->opt_extruder_colour_int = nullptr;
    this->opt_filament_colour_int = nullptr;
    this->num_extruders = 0;
    this->position.clear();
    this->e_position.clear();
    this->e_retracted.clear();
    this->e_restart_extra.clear();
}

void GCodeGenerator::PlaceholderParserIntegration::init(const PrintConfig &print_config, const GCodeWriter &writer)
{
    this->reset();
    const std::vector<Extruder> &extruders = writer.extruders();
    if (! extruders.empty()) {
        this->num_extruders = extruders.back().id() + 1;
        this->e_retracted.assign(num_extruders, 0);
        this->e_restart_extra.assign(num_extruders, 0);
        this->opt_e_retracted = new ConfigOptionFloats(e_retracted);
        this->opt_e_restart_extra = new ConfigOptionFloats(e_restart_extra);
        this->output_config.set_key_value("e_retracted", this->opt_e_retracted);
        this->output_config.set_key_value("e_restart_extra", this->opt_e_restart_extra);
        if (! writer.config.use_relative_e_distances) {
            e_position.assign(num_extruders, 0);
            opt_e_position = new ConfigOptionFloats(e_position);
            this->output_config.set_key_value("e_position", opt_e_position);
        }
    }
    this->opt_extruded_volume = new ConfigOptionFloats(this->num_extruders, 0.f);
    this->opt_extruded_weight = new ConfigOptionFloats(this->num_extruders, 0.f);
    this->opt_extruded_volume_total = new ConfigOptionFloat(0.f);
    this->opt_extruded_weight_total = new ConfigOptionFloat(0.f);
    this->parser.set("extruded_volume", this->opt_extruded_volume);
    this->parser.set("extruded_weight", this->opt_extruded_weight);
    this->parser.set("extruded_volume_total", this->opt_extruded_volume_total);
    this->parser.set("extruded_weight_total", this->opt_extruded_weight_total);
    
    // colors
    this->opt_filament_colour_int = new ConfigOptionInts(this->num_extruders, 0);
    this->opt_extruder_colour_int = new ConfigOptionInts(this->num_extruders, 0);
    for (const Extruder &e : writer.extruders()) {
        //std::string  
        Slic3r::ColorRGB color;
        // filament_colour -> filament_colour_int
        std::string  str_val = print_config.filament_colour.get_at(e.id()); // should it move into gcode config?
        if (decode_color(str_val, color)) {
            int32_t rgb_int = int32_t(color.r_uchar());
            rgb_int = (rgb_int << 8) + int32_t(color.g_uchar());
            rgb_int = (rgb_int << 8) + int32_t(color.b_uchar());
            this->opt_filament_colour_int->get_at(e.id()) = rgb_int;
        }
        // extruder_colour -> extruder_colour_int
        str_val = print_config.extruder_colour.get_at(e.id()); // should it move into gcode config?
        if (decode_color(str_val, color)) {
            int32_t rgb_int = int32_t(color.r_uchar());
            rgb_int = (rgb_int << 8) + int32_t(color.g_uchar());
            rgb_int = (rgb_int << 8) + int32_t(color.b_uchar());
            this->opt_extruder_colour_int->get_at(e.id()) = rgb_int;
        }
    }
    this->parser.set("filament_colour_int", this->opt_filament_colour_int);
    this->parser.set("extruder_colour_int", this->opt_extruder_colour_int);

    // Reserve buffer for current position.
    this->position.assign(3, 0);
    this->opt_position = new ConfigOptionFloats(this->position);
    this->output_config.set_key_value("position", this->opt_position);
    this->opt_position_parser = new ConfigOptionFloats(this->position);
    this->parser.set("current_position", this->opt_position_parser);

    // Store zhop variable into the parser itself, it is a read-only variable to the script.
    this->opt_zhop = new ConfigOptionFloat(0);
    this->parser.set("zhop", this->opt_zhop);
}

void GCodeGenerator::PlaceholderParserIntegration::update_from_gcodewriter(const GCodeWriter &writer, const WipeTowerData& wipe_tower_data)
{
    memcpy(this->position.data(), writer.get_position().data(), sizeof(double) * 3);
    this->opt_position->set(this->position);
    this->opt_position_parser->set(this->position);
    this->opt_zhop->value = writer.get_lift();

    if (this->num_extruders > 0) {
        const std::vector<Extruder> &extruders = writer.extruders();
        assert(! extruders.empty() && num_extruders == extruders.back().id() + 1);
        this->e_retracted.assign(num_extruders, 0);
        this->e_restart_extra.assign(num_extruders, 0);
        this->opt_extruded_volume->set(std::vector<double>(num_extruders, 0));
        this->opt_extruded_weight->set(std::vector<double>(num_extruders, 0));
        assert(this->opt_extruded_weight->size() == num_extruders);
        double total_volume = 0.;
        double total_weight = 0.;
        for (const Extruder &e : extruders) {
            this->e_retracted[e.id()]     = e.retracted();
            this->e_restart_extra[e.id()] = e.restart_extra();

            // Wipe tower filament consumption has to be added separately, because that gcode is not generated by GCodeWriter.
            double wt_vol = 0.;
            const std::vector<std::pair<float, std::vector<float>>>& wtuf = wipe_tower_data.used_filament_until_layer;
            if (!wtuf.empty()) {
                auto it = std::lower_bound(wtuf.begin(), wtuf.end(), writer.get_position().z(),
                                [](const auto& a, const float& val) { return a.first < val; });
                if (it == wtuf.end())
                    it = wtuf.end() - 1;
                wt_vol = it->second[e.id()] * e.filament_crossection();
            }            

            double v = e.extruded_volume() + wt_vol;
            double w = v * e.filament_density() * 0.001;
            this->opt_extruded_volume->get_at(e.id()) = v;
            this->opt_extruded_weight->get_at(e.id()) = w;
            total_volume += v;
            total_weight += w;
        }
        opt_extruded_volume_total->value = total_volume;
        opt_extruded_weight_total->value = total_weight;
        opt_e_retracted->set(this->e_retracted);
        opt_e_restart_extra->set(this->e_restart_extra);
        if (! writer.config.use_relative_e_distances) {
            this->e_position.assign(num_extruders, 0);
            for (const Extruder &e : extruders)
                this->e_position[e.id()] = e.position();
            this->opt_e_position->set(this->e_position);
        }
    }
}

// Throw if any of the output vector variables were resized by the script.
void GCodeGenerator::PlaceholderParserIntegration::validate_output_vector_variables()
{
    if (this->opt_position->size() != 3)
        throw Slic3r::RuntimeError("\"position\" output variable must not be resized by the script.");
    if (this->num_extruders > 0) {
        if (this->opt_e_position && this->opt_e_position->size() != this->num_extruders)
            throw Slic3r::RuntimeError("\"e_position\" output variable must not be resized by the script.");
        if (this->opt_e_retracted->size() != this->num_extruders)
            throw Slic3r::RuntimeError("\"e_retracted\" output variable must not be resized by the script.");
        if (this->opt_e_restart_extra->size() != this->num_extruders)
            throw Slic3r::RuntimeError("\"e_restart_extra\" output variable must not be resized by the script.");
    }
}

constexpr float SMALL_PERIMETER_SPEED_RATIO_OFFSET = (-10);

// Collect pairs of object_layer + support_layer sorted by print_z.
// object_layer & support_layer are considered to be on the same print_z, if they are not further than EPSILON.
GCodeGenerator::ObjectsLayerToPrint GCodeGenerator::collect_layers_to_print(const PrintObject &object, Print::StatusMonitor &status_monitor)
{
    GCodeGenerator::ObjectsLayerToPrint layers_to_print;
    layers_to_print.reserve(object.layers().size() + object.support_layers().size());

    /*
    // Calculate a minimum support layer height as a minimum over all extruders, but not smaller than 10um.
    // This is the same logic as in support generator.
    //FIXME should we use the printing extruders instead?
    double gap_over_supports = object.config().support_material_contact_distance_top;
    // FIXME should we test object.config().support_material_synchronize_layers ? IN prusa code, the support layers are synchronized with object layers iff soluble supports.
    //assert(!object.config().object.has_support() || gap_over_supports != 0. || object.config().support_material_synchronize_layers);
    if (gap_over_supports != 0.) {
        gap_over_supports = std::max(0., gap_over_supports);
        // Not a soluble support,
        double support_layer_height_min = 1000000.;
        const ConfigOptionFloatsOrPercents& min_layer_height = object.print()->config().min_layer_height;
        const ConfigOptionFloats& nozzle_diameter = object.print()->config().nozzle_diameter;
        for(int extr_id = 0; extr_id < min_layer_height.size(); ++extr_id)
            support_layer_height_min = std::min(support_layer_height_min, std::max(nozzle_diameter.get_at(extr_id)/40, min_layer_height.get_abs_value(extr_id, nozzle_diameter.get_at(extr_id))));
        gap_over_supports += support_layer_height_min;
    }*/

    std::vector<std::pair<double, double>> warning_ranges;

    //check for max nozzle diameter
    const std::vector<double>& nozzle_diameters = object.print()->config().nozzle_diameter.get_values();
    std::set<uint16_t> exctruder_ids = object.object_extruders();
    double max_nozzle = 0;
    for (uint16_t id : exctruder_ids) {
        max_nozzle = std::max(max_nozzle, nozzle_diameters[id]);
    }
    if (max_nozzle == 0)
        max_nozzle = nozzle_diameters.front();
    double top_cd = object.config().support_material_contact_distance.get_abs_value(max_nozzle);
    double bottom_cd = object.config().support_material_bottom_contact_distance.value == 0. ? top_cd : object.config().support_material_bottom_contact_distance.get_abs_value(max_nozzle);
    double raft_cd = object.config().raft_contact_distance.value;

    // Pair the object layers with the support layers by z.
    size_t idx_object_layer  = 0;
    size_t idx_support_layer = 0;
    const ObjectLayerToPrint* last_extrusion_layer = nullptr;
    while (idx_object_layer < object.layers().size() || idx_support_layer < object.support_layers().size()) {
        ObjectLayerToPrint layer_to_add_to_print;
        layer_to_add_to_print.object_layer = (idx_object_layer < object.layers().size()) ? object.layers()[idx_object_layer++] : nullptr;
        layer_to_add_to_print.support_layer = (idx_support_layer < object.support_layers().size()) ? object.support_layers()[idx_support_layer++] : nullptr;
        if (layer_to_add_to_print.object_layer && layer_to_add_to_print.support_layer) {
            if (layer_to_add_to_print.object_layer->print_z < layer_to_add_to_print.support_layer->print_z - EPSILON) {
                layer_to_add_to_print.support_layer = nullptr;
                --idx_support_layer;
            }
            else if (layer_to_add_to_print.support_layer->print_z < layer_to_add_to_print.object_layer->print_z - EPSILON) {
                layer_to_add_to_print.object_layer = nullptr;
                --idx_object_layer;
            }
        }

        layers_to_print.push_back(std::move(layer_to_add_to_print));
        const ObjectLayerToPrint& layer_to_print = layers_to_print.back();

        bool has_extrusions = (layer_to_print.object_layer && layer_to_print.object_layer->has_extrusions())
            || (layer_to_print.support_layer && layer_to_print.support_layer->has_extrusions());

        // Check that there are extrusions on the very first layer. The case with empty
        // first layer may result in skirt/brim in the air and maybe other issues.
        if (layers_to_print.size() == 1u && !object.print()->config().allow_empty_layers.value) {
            if (!has_extrusions)
                throw Slic3r::SlicingError(_u8L("There is an object with no extrusions in the first layer.") + "\n" +
                                           _u8L("Object name") + ": " + object.model_object()->name);
        }

        // In case there are extrusions on this layer, check there is a layer to lay it on.
        if ((layer_to_print.object_layer && layer_to_print.object_layer->has_extrusions())
            // Allow empty support layers, as the support generator may produce no extrusions for non-empty support regions.
         || (layer_to_print.support_layer /* && layer_to_print.support_layer->has_extrusions() */)) {

            double extra_gap = (layer_to_print.support_layer ? bottom_cd : top_cd);
            if (object.config().raft_layers.value > 0 && layer_to_print.layer()->id() <= object.config().raft_layers.value) {
                extra_gap = raft_cd;
            }
            if (object.config().support_material_contact_distance_type.value == SupportZDistanceType::zdNone) {
                extra_gap = layer_to_print.layer()->height;
            } else if (object.config().support_material_contact_distance_type.value == SupportZDistanceType::zdFilament) {
                //compute the height of bridge.
                if (layer_to_print.layer()->id() > 0 && !layer_to_print.layer()->regions().empty()) {
                    extra_gap += layer_to_print.layer()->regions().front()->bridging_flow(FlowRole::frSolidInfill).height();
                } else {
                    extra_gap += layer_to_print.layer()->height;
                }
            } else { //SupportZDistanceType::zdPlane
                extra_gap += layer_to_print.layer()->height;
            }

            double maximal_print_z = check_z_step(
                (last_extrusion_layer ? last_extrusion_layer->print_z() : 0.) + std::max(0., extra_gap),
                object.print()->config().z_step);
            // Negative support_contact_z is not taken into account, it can result in false positives in cases
            // where previous layer has object extrusions too (https://github.com/prusa3d/PrusaSlicer/issues/2752)

            if (has_extrusions && !object.print()->config().allow_empty_layers && layer_to_print.print_z() > maximal_print_z + 2. * EPSILON
                //don't check for raft layers: there is an empty space between the last raft and the first layer
                //&& (object.config().raft_layers.value == 0 || (layer_to_print.object_layer && layer_to_print.object_layer->id() > object.config().raft_layers.value)) 
                )
                warning_ranges.emplace_back(std::make_pair((last_extrusion_layer ? last_extrusion_layer->print_z() : 0.), layers_to_print.back().print_z()));
        }
        // Remember last layer with extrusions.
        if (has_extrusions)
            last_extrusion_layer = &layers_to_print.back();
    }

    if (! warning_ranges.empty()) {
        std::string warning;
        size_t i = 0;
        for (i = 0; i < std::min(warning_ranges.size(), size_t(3)); ++i)
            warning += Slic3r::format(_u8L("Empty layer between %1% and %2%."),
                                      warning_ranges[i].first, warning_ranges[i].second) + "\n";
        if (i < warning_ranges.size())
            warning += _u8L("(Some lines not shown)") + "\n";
        warning += "\n";
        warning += Slic3r::format(_u8L("Object name: %1%"), object.model_object()->name) + "\n\n"
            + _u8L("Make sure the object is printable. This is usually caused by negligibly small extrusions or by a faulty model. "
                "Try to repair the model or change its orientation on the bed.");

        status_monitor.active_step_add_warning(
            PrintStateBase::WarningLevel::CRITICAL, warning);
    }

    return layers_to_print;
}

// Prepare for non-sequential printing of multiple objects: Support resp. object layers with nearly identical print_z
// will be printed for  all objects at once.
// Return a list of <print_z, per object ObjectLayerToPrint> items.
std::vector<std::pair<coordf_t, GCodeGenerator::ObjectsLayerToPrint>> GCodeGenerator::collect_layers_to_print(const Print &print, Print::StatusMonitor &status_monitor)
{
    struct OrderingItem {
        coordf_t    print_z;
        size_t      object_idx;
        size_t      layer_idx;
    };

    std::vector<ObjectsLayerToPrint>  per_object(print.objects().size(), ObjectsLayerToPrint());
    std::vector<OrderingItem>               ordering;
    for (size_t i = 0; i < print.objects().size(); ++i) {
        per_object[i] = collect_layers_to_print(*print.objects()[i], status_monitor);
        OrderingItem ordering_item;
        ordering_item.object_idx = i;
        ordering.reserve(ordering.size() + per_object[i].size());
        const ObjectLayerToPrint& front = per_object[i].front();
        for (const ObjectLayerToPrint& ltp : per_object[i]) {
            ordering_item.print_z   = ltp.print_z();
            ordering_item.layer_idx = &ltp - &front;
            ordering.push_back(ordering_item);
        }
    }

    std::sort(ordering.begin(), ordering.end(), [](const OrderingItem& oi1, const OrderingItem& oi2) { return oi1.print_z < oi2.print_z; });

    std::vector<std::pair<coordf_t, ObjectsLayerToPrint>> layers_to_print;

    // Merge numerically very close Z values.
    for (size_t i = 0; i < ordering.size();) {
        // Find the last layer with roughly the same print_z.
        size_t j = i + 1;
        coordf_t zmax = ordering[i].print_z + EPSILON;
        for (; j < ordering.size() && ordering[j].print_z <= zmax; ++j);
        // Merge into layers_to_print.
        std::pair<coordf_t, ObjectsLayerToPrint> merged;
        // Assign an average print_z to the set of layers with nearly equal print_z.
        merged.first = 0.5 * (ordering[i].print_z + ordering[j - 1].print_z);
        merged.second.assign(print.objects().size(), ObjectLayerToPrint());
        for (; i < j; ++ i) {
            const OrderingItem& oi = ordering[i];
            assert(merged.second[oi.object_idx].layer() == nullptr);
            merged.second[oi.object_idx] = std::move(per_object[oi.object_idx][oi.layer_idx]);
        }
        layers_to_print.push_back(std::move(merged));
    }

    return layers_to_print;
}

// free functions called by GCodeGenerator::do_export()
namespace DoExport {
//    static void update_print_estimated_times_stats(const GCodeProcessor& processor, PrintStatistics& print_statistics)
//    {
//        const GCodeProcessorResult& result = processor.get_result();
//        print_statistics.estimated_normal_print_time = get_time_dhms(result.print_statistics.modes[static_cast<size_t>(PrintEstimatedStatistics::ETimeMode::Normal)].time);
//        print_statistics.estimated_silent_print_time = processor.is_stealth_time_estimator_enabled() ?
//            get_time_dhms(result.print_statistics.modes[static_cast<size_t>(PrintEstimatedStatistics::ETimeMode::Stealth)].time) : "N/A";
//    }

    static void update_print_estimated_stats(const GCodeProcessor& processor, const std::vector<Extruder>& extruders, const PrintConfig& config, PrintStatistics& print_statistics)
    {
        const GCodeProcessorResult& result = processor.get_result();
        print_statistics.estimated_print_time.clear();
        print_statistics.estimated_print_time_str.clear();
        print_statistics.estimated_print_time[static_cast<uint8_t>(PrintEstimatedStatistics::ETimeMode::Normal)] =
            result.print_statistics.modes[static_cast<size_t>(PrintEstimatedStatistics::ETimeMode::Normal)].time;
        print_statistics.estimated_print_time_str[static_cast<uint8_t>(PrintEstimatedStatistics::ETimeMode::Normal)] =
            get_time_dhms(result.print_statistics.modes[static_cast<size_t>(PrintEstimatedStatistics::ETimeMode::Normal)].time);
        if(processor.is_stealth_time_estimator_enabled()){
            print_statistics.estimated_print_time[static_cast<uint8_t>(PrintEstimatedStatistics::ETimeMode::Stealth)] =
                result.print_statistics.modes[static_cast<size_t>(PrintEstimatedStatistics::ETimeMode::Stealth)].time;
            print_statistics.estimated_print_time_str[static_cast<uint8_t>(PrintEstimatedStatistics::ETimeMode::Stealth)] =
                get_time_dhms(result.print_statistics.modes[static_cast<size_t>(PrintEstimatedStatistics::ETimeMode::Stealth)].time);
        }

        // update filament statictics
        double total_extruded_volume = 0.0;
        double total_used_filament   = 0.0;
        double total_weight          = 0.0;
        double total_cost            = 0.0;
        for (auto volume : result.print_statistics.volumes_per_extruder) {
            total_extruded_volume += volume.second;

            size_t extruder_id = volume.first;
            auto extruder = std::find_if(extruders.begin(), extruders.end(), [extruder_id](const Extruder& extr) { return extr.id() == extruder_id; });
            if (extruder == extruders.end())
                continue;

            double section = PI * sqr(0.5 * extruder->filament_diameter());
            double weight = volume.second * extruder->filament_density() * 0.001;
            total_used_filament += volume.second / section;
            total_weight        += weight;
            total_cost          += weight * extruder->filament_cost() * 0.001;
        }
        total_cost += config.time_cost.value * (processor.get_time(PrintEstimatedStatistics::ETimeMode::Normal) / 3600.f);

        print_statistics.total_extruded_volume = total_extruded_volume;
        print_statistics.total_used_filament   = total_used_filament;
        print_statistics.total_weight          = total_weight;
        print_statistics.total_cost            = total_cost;

        print_statistics.filament_stats        = result.print_statistics.volumes_per_extruder;
    }

    // if any reserved keyword is found, returns a std::vector containing the first MAX_COUNT keywords found
    // into pairs containing:
    // first: source
    // second: keyword
    // to be shown in the warning notification
    // The returned vector is empty if no keyword has been found
    static std::vector<std::pair<std::string, std::string>> validate_custom_gcode(const Print& print) {
        static const unsigned int MAX_TAGS_COUNT = 5;
        std::vector<std::pair<std::string, std::string>> ret;

        auto check = [&ret](const std::string& source, const std::string& gcode) {
            std::vector<std::string> tags;
            if (GCodeProcessor::contains_reserved_tags(gcode, MAX_TAGS_COUNT, tags)) {
                if (!tags.empty()) {
                    size_t i = 0;
                    while (ret.size() < MAX_TAGS_COUNT && i < tags.size()) {
                        ret.push_back({ source, tags[i] });
                        ++i;
                    }
                }
            }
        };

        const GCodeConfig& config = print.config();
        check(_u8L("Start G-code"), config.start_gcode.value);
        if (ret.size() < MAX_TAGS_COUNT) check(_u8L("End G-code"), config.end_gcode.value);
        if (ret.size() < MAX_TAGS_COUNT) check(_u8L("Before layer change G-code"), config.before_layer_gcode.value);
        if (ret.size() < MAX_TAGS_COUNT) check(_u8L("After layer change G-code"), config.layer_gcode.value);
        if (ret.size() < MAX_TAGS_COUNT) check(_u8L("Extrusion type change G-code"), config.feature_gcode.value);
        if (ret.size() < MAX_TAGS_COUNT) check(_u8L("Tool change G-code"), config.toolchange_gcode.value);
        if (ret.size() < MAX_TAGS_COUNT) check(_u8L("Between objects G-code (for sequential printing"), config.between_objects_gcode.value);
        if (ret.size() < MAX_TAGS_COUNT) check(_u8L("Color Change G-code"), GCodeWriter::get_default_color_change_gcode(config));
        if (ret.size() < MAX_TAGS_COUNT) check(_u8L("Pause Print G-code"), GCodeWriter::get_default_pause_gcode(config));
        if (ret.size() < MAX_TAGS_COUNT) check(_u8L("Template Custom G-code"), config.template_custom_gcode.value);
        if (ret.size() < MAX_TAGS_COUNT) {
            for (const std::string& value : config.start_filament_gcode.get_values()) {
                check(_u8L("Filament Start G-code"), value);
                if (ret.size() == MAX_TAGS_COUNT)
                    break;
            }
        }
        if (ret.size() < MAX_TAGS_COUNT) {
            for (const std::string& value : config.end_filament_gcode.get_values()) {
                check(_u8L("Filament End G-code"), value);
                if (ret.size() == MAX_TAGS_COUNT)
                    break;
            }
        }
        if (ret.size() < MAX_TAGS_COUNT) {
            for (const std::string& value : print.config().milling_toolchange_start_gcode.get_values()) {
                check(_u8L("Milling Start G-code"), value);
                if (ret.size() == MAX_TAGS_COUNT)
                    break;
            }
        }
        if (ret.size() < MAX_TAGS_COUNT) {
            for (const std::string& value : print.config().milling_toolchange_end_gcode.get_values()) {
                check(_u8L("Milling End G-code"), value);
                if (ret.size() == MAX_TAGS_COUNT)
                    break;
            }
        }
        if (ret.size() < MAX_TAGS_COUNT) {
            const CustomGCode::Info& custom_gcode_per_print_z = print.model().custom_gcode_per_print_z;
            for (const auto& gcode : custom_gcode_per_print_z.gcodes) {
                check(_u8L("Custom G-code"), gcode.extra);
                if (ret.size() == MAX_TAGS_COUNT)
                    break;
            }
        }
        if (ret.size() < MAX_TAGS_COUNT) {
            std::set<std::string> per_object_gcodes;
            for (const PrintObject *obj : print.objects())
                per_object_gcodes.insert(obj->config().object_gcode.value);
            for (const std::string &gcode : per_object_gcodes) {
                check(_u8L("Per object G-code"), gcode);
                if (ret.size() == MAX_TAGS_COUNT)
                    break;
            }
        }

        return ret;
    }
} // namespace DoExport


void check_remaning_times(GCodeFlavor firmware, RemainingTimeType type, Print::StatusMonitor &monitor)
{
    if (type == RemainingTimeType::rtM73 || type == rtM73_M117 || type == rtM73_Quiet) {
        if (!(firmware == gcfMarlinLegacy || firmware == gcfMarlinFirmware || firmware == gcfMakerWare ||
                firmware == gcfKlipper || firmware == gcfRepRap))
            monitor.active_step_add_warning(
                PrintStateBase::WarningLevel::CRITICAL,
                L("Your firmware doesn't allow to print the remaining times with M73."));
    }
    if ((type == rtM117 || type == rtM73_M117)) {
        if (!(firmware == gcfMarlinLegacy || firmware == gcfMarlinFirmware || firmware == gcfMakerWare ||
                firmware == gcfKlipper || firmware == gcfRepRap || firmware == gcfRepetier ||
                firmware == gcfSmoothie))
            monitor.active_step_add_warning(
                PrintStateBase::WarningLevel::CRITICAL,
                L("Your firmware doesn't allow to print the remaining times with M117."));
    }
}

GCodeGenerator::GCodeGenerator() :
    m_origin(Vec2d::Zero()),
    m_enable_loop_clipping(true), 
    m_enable_cooling_markers(false), 
    m_enable_extrusion_role_markers(false),
    m_last_processor_extrusion_role(GCodeExtrusionRole::None),
    m_layer_count(0),
    m_layer_with_support_count(0),
    m_layer_index(-1), 
    m_layer(nullptr),
    m_object_layer_over_raft(false),
    m_volumetric_speed(0),
    m_last_extrusion_role(GCodeExtrusionRole::None),
    m_last_width(0.0f),
#if ENABLE_GCODE_VIEWER_DATA_CHECKING
    m_last_mm3_per_mm(0.0),
#endif // ENABLE_GCODE_VIEWER_DATA_CHECKING
    m_brim_done(false),
    m_second_layer_things_done(false),
    m_silent_time_estimator_enabled(false),
    m_current_instance({nullptr, -1}),
    m_last_too_small(ExtrusionPath{ExtrusionAttributes{ExtrusionRole::None}})
    {
        cooldown_marker_init();
    }

void GCodeGenerator::do_export(Print* print, const char* path, GCodeProcessorResult* result, ThumbnailsGeneratorCallback thumbnail_cb)
{

    // mutable print status, to make print unmutable.
    Print::StatusMonitor monitor{ *print };

    CNumericLocalesSetter locales_setter;

    // Does the file exist? If so, we hope that it is still valid.
    {
        PrintStateBase::StateWithTimeStamp state = print->step_state_with_timestamp(psGCodeExport);
        if (! state.enabled || (state.is_done() && boost::filesystem::exists(boost::filesystem::path(path))))
            return;
    }
    
    // Enabled and either not done, or marked as done while the output file is missing.
    monitor.set_started(psGCodeExport);

    // check if any custom gcode contains keywords used by the gcode processor to
    // produce time estimation and gcode toolpaths
    std::vector<std::pair<std::string, std::string>> validation_res = DoExport::validate_custom_gcode(*print);
    if (!validation_res.empty()) {
        std::string reports;
        for (const auto& [source, keyword] : validation_res) {
            reports += source + ": \"" + keyword + "\"\n";
        }
        monitor.active_step_add_warning(PrintStateBase::WarningLevel::NON_CRITICAL,
            _u8L("In the custom G-code were found reserved keywords:") + "\n" +
            reports +
            _u8L("This may cause problems in g-code visualization and printing time estimation."));
    }

    //check if the precision is high enough to not cause problems (extrusion of 0 filament length)
    {
        double xyz_precision = std::pow(0.1,print->config().gcode_precision_xyz.value);
        double e_precision = std::pow(0.1,print->config().gcode_precision_e.value);
        double min_diameter = print->config().nozzle_diameter.get_at(0);
        for (size_t i = 1; i < print->config().nozzle_diameter.size(); ++i)
            min_diameter = std::min(min_diameter, print->config().nozzle_diameter.get_at(0));
        if (xyz_precision > min_diameter / 10)
            throw Slic3r::RuntimeError(std::string("Error: 'gcode_precision_xyz' is too imprecise for a nozzle diameter of ") + std::to_string(min_diameter));
        if (e_precision > min_diameter / 50)
            throw Slic3r::RuntimeError(std::string("Error: 'gcode_precision_e' is too imprecise for a nozzle diameter of ") + std::to_string(min_diameter));
    }


    BOOST_LOG_TRIVIAL(info) << "Exporting G-code..." << log_memory_info();

    // Remove the old g-code if it exists.
    boost::nowide::remove(path);

    std::string path_tmp(path);
    path_tmp += ".tmp";

    m_processor.initialize(path_tmp);
    m_processor.set_status_monitor(&monitor);
    m_processor.get_binary_data() = bgcode::binarize::BinaryData();
    GCodeOutputStream file(boost::nowide::fopen(path_tmp.c_str(), "wb"), m_processor);
    if (! file.is_open())
        throw Slic3r::RuntimeError(std::string("G-code export to ") + path + " failed.\nCannot open the file for writing.\n");
    
    if(print->config().remaining_times)
        check_remaning_times(print->config().gcode_flavor, print->config().remaining_times_type, monitor);

    try {
        this->_do_export(*print, file, thumbnail_cb);
        file.flush();
        if (file.is_error()) {
            file.close();
            boost::nowide::remove(path_tmp.c_str());
            throw Slic3r::RuntimeError(std::string("G-code export to ") + path + " failed\nIs the disk full?\n");
        }
    } catch (std::exception & /* ex */) {
        // Rethrow on any exception. std::runtime_exception and CanceledException are expected to be thrown.
        // Close and remove the file.
        file.close();
        boost::nowide::remove(path_tmp.c_str());
        throw;
    }
    file.close();

    if (! this->m_placeholder_parser_integration.failed_templates.empty()) {
        // G-code export proceeded, but some of the PlaceholderParser substitutions failed.
        //FIXME localize!
        std::string msg = std::string("G-code export to ") + path + " failed due to invalid custom G-code sections:\n\n";
        for (const auto &name_and_error : this->m_placeholder_parser_integration.failed_templates)
            msg += name_and_error.first + "\n" + name_and_error.second + "\n";
        msg += "\nPlease inspect the file ";
        msg += path_tmp + " for ExtrusionRole::ror messages enclosed between\n";
        msg += "        !!!!! Failed to process the custom G-code template ...\n";
        msg += "and\n";
        msg += "        !!!!! End of an ExtrusionRole::ror report for the custom G-code template ...\n";
        msg += "for all macro processing ExtrusionRole::rors.";
        throw Slic3r::PlaceholderParserError(msg);
    }

    BOOST_LOG_TRIVIAL(debug) << "Start processing gcode, " << log_memory_info();
    // Post-process the G-code to update time stamps.
    m_processor.finalize(true);
//    DoExport::update_print_estimated_times_stats(m_processor, print->m_print_statistics);
    DoExport::update_print_estimated_stats(m_processor, m_writer.extruders(), print->config(), monitor.stats());
    if (result != nullptr) {
        *result = std::move(m_processor.extract_result());
        // set the filename to the correct value
        result->filename = path;
    }
    BOOST_LOG_TRIVIAL(debug) << "Finished processing gcode, " << log_memory_info();

    if (rename_file(path_tmp, path)) {
        std::string err_msg = ("Failed to rename the output G-code file from " + path_tmp + " to " + path + '\n' + "Is " + path_tmp + " locked?" + '\n');
        if (copy_file(path_tmp, path, err_msg, true) != CopyFileResult::SUCCESS)
            throw Slic3r::RuntimeError(err_msg);
    }

    BOOST_LOG_TRIVIAL(info) << "Exporting G-code finished" << log_memory_info();
    monitor.set_done(psGCodeExport);
    //notify gui that the gcode is ready to be drawed
    print->set_status(100, "", PrintBase::SlicingStatus::DEFAULT | PrintBase::SlicingStatus::SECONDARY_STATE);
    print->set_status(100, L("Gcode done"), PrintBase::SlicingStatus::FlagBits::GCODE_ENDED);
    
    m_processor.set_status_monitor(nullptr);
}

// free functions called by GCodeGenerator::_do_export()
namespace DoExport {

    class ExtrusionMinMM : public ExtrusionVisitorConst {
        double min = std::numeric_limits<double>::max();
        std::unordered_set<ExtrusionRole> excluded;
    public:
        ExtrusionMinMM(const ConfigBase* config) {
            excluded.insert(ExtrusionRole::Ironing);
            excluded.insert(ExtrusionRole::Milling);
            excluded.insert(ExtrusionRole::Mixed);
            excluded.insert(ExtrusionRole::None);
            excluded.insert(ExtrusionRole::WipeTower);
            if (config->option("perimeter_speed") != nullptr && config->get_computed_value("perimeter_speed") != 0)
                excluded.insert(ExtrusionRole::Perimeter);
            if (config->option("external_perimeter_speed") != nullptr && config->get_computed_value("external_perimeter_speed") != 0)
                excluded.insert(ExtrusionRole::ExternalPerimeter);
            if (config->option("overhangs_speed") != nullptr && config->get_computed_value("overhangs_speed") != 0) {
                excluded.insert(ExtrusionRole::OverhangPerimeter);
                excluded.insert(ExtrusionRole::OverhangExternalPerimeter);
            }
            if (config->option("gap_fill_speed") != nullptr && config->get_computed_value("gap_fill_speed") != 0)
                excluded.insert(ExtrusionRole::GapFill);
            if (config->option("thin_walls_speed") != nullptr && config->get_computed_value("thin_walls_speed") != 0)
                excluded.insert(ExtrusionRole::ThinWall);
            if (config->option("infill_speed") != nullptr && config->get_computed_value("infill_speed") != 0)
                excluded.insert(ExtrusionRole::InternalInfill);
            if (config->option("solid_infill_speed") != nullptr && config->get_computed_value("solid_infill_speed") != 0)
                excluded.insert(ExtrusionRole::SolidInfill);
            if (config->option("top_solid_infill_speed") != nullptr && config->get_computed_value("top_solid_infill_speed") != 0)
                excluded.insert(ExtrusionRole::TopSolidInfill);
            if (config->option("bridge_speed") != nullptr && config->get_computed_value("bridge_speed") != 0)
                excluded.insert(ExtrusionRole::BridgeInfill);
            if (config->option("internal_bridge_speed") != nullptr && config->get_computed_value("internal_bridge_speed") != 0)
                excluded.insert(ExtrusionRole::InternalBridgeInfill);
            if (config->option("support_material_speed") != nullptr && config->get_computed_value("support_material_speed") != 0)
                excluded.insert(ExtrusionRole::SupportMaterial);
            if (config->option("support_material_interface_speed") != nullptr && config->get_computed_value("support_material_interface_speed") != 0)
                excluded.insert(ExtrusionRole::SupportMaterialInterface);
            if (config->option("brim_speed") != nullptr && config->get_computed_value("brim_speed") != 0)
                excluded.insert(ExtrusionRole::Skirt);
        }
        virtual void use(const ExtrusionPath& path) override {
            if (excluded.find(path.role()) == excluded.end())
                min = std::min(min, path.mm3_per_mm());
        }
        virtual void use(const ExtrusionPath3D& path3D) override {
            if (excluded.find(path3D.role()) == excluded.end())
                min = std::min(min, path3D.mm3_per_mm());
        }
        virtual void use(const ExtrusionMultiPath& multipath) override {
            for (const ExtrusionPath& path : multipath.paths)
                use(path);
        }
        virtual void use(const ExtrusionMultiPath3D& multipath) override {
            for (const ExtrusionPath& path : multipath.paths)
                use(path);
        }
        virtual void use(const ExtrusionLoop& loop) override {
            for (const ExtrusionPath& path : loop.paths)
                use(path);
        }
        virtual void use(const ExtrusionEntityCollection& collection) override {
            for (const ExtrusionEntity* entity : collection.entities())
                entity->visit(*this);
        }
        double reset_use_get(const ExtrusionEntityCollection entity) { reset(); use(entity); return get(); }
        double get() { return min; }
        void reset() { min = std::numeric_limits<double>::max(); }
        //test if at least a ExtrusionRole from tests is used for min computation
        bool is_compatible(std::initializer_list<ExtrusionRole> tests) { 
            for (ExtrusionRole test : tests)
                if (excluded.find(test) == excluded.end())
                    return true;
            return false;
        }
    };

    static void init_gcode_processor(const PrintConfig& config, GCodeProcessor& processor, bool& silent_time_estimator_enabled)
    {
        silent_time_estimator_enabled = (config.gcode_flavor.value == gcfMarlinLegacy || config.gcode_flavor.value == gcfMarlinFirmware)
                                        && config.silent_mode;
        processor.reset();
        processor.initialize_result_moves();
        processor.apply_config(config);
        processor.enable_stealth_time_estimator(silent_time_estimator_enabled);
    }

	static double autospeed_volumetric_limit(const Print &print)
	{
        ExtrusionMinMM compute_min_mm3_per_mm{ &print.full_print_config() };
	    // get the minimum cross-section used in the print
	    std::vector<double> mm3_per_mm;
	    for (auto object : print.objects()) {
	        for (size_t region_id = 0; region_id < object->num_printing_regions(); ++ region_id) {
	            const PrintRegion &region = object->printing_region(region_id);
	            for (auto layer : object->layers()) {
	                const LayerRegion* layerm = layer->regions()[region_id];
                    if (compute_min_mm3_per_mm.is_compatible({ ExtrusionRole::Perimeter, ExtrusionRole::ExternalPerimeter, ExtrusionRole::OverhangPerimeter, ExtrusionRole::OverhangExternalPerimeter }))
                        mm3_per_mm.push_back(compute_min_mm3_per_mm.reset_use_get(layerm->perimeters()));
                    if (compute_min_mm3_per_mm.is_compatible({ ExtrusionRole::InternalInfill, ExtrusionRole::SolidInfill, ExtrusionRole::TopSolidInfill,ExtrusionRole::BridgeInfill,ExtrusionRole::InternalBridgeInfill }))
                        mm3_per_mm.push_back(compute_min_mm3_per_mm.reset_use_get(layerm->fills()));
	            }
	        }
            if (compute_min_mm3_per_mm.is_compatible({ ExtrusionRole::SupportMaterial, ExtrusionRole::SupportMaterialInterface }))
	            for (auto layer : object->support_layers())
                    mm3_per_mm.push_back(compute_min_mm3_per_mm.reset_use_get(layer->support_fills));
	    }
        if (compute_min_mm3_per_mm.is_compatible({ ExtrusionRole::Skirt })) {
            mm3_per_mm.push_back(compute_min_mm3_per_mm.reset_use_get(print.skirt()));
            if(print.skirt_first_layer())
                mm3_per_mm.push_back(compute_min_mm3_per_mm.reset_use_get(*print.skirt_first_layer()));
            mm3_per_mm.push_back(compute_min_mm3_per_mm.reset_use_get(print.brim()));
        }
	    // filter out 0-width segments
	    mm3_per_mm.erase(std::remove_if(mm3_per_mm.begin(), mm3_per_mm.end(), [](double v) { return v < 0.000001; }), mm3_per_mm.end());
	    double volumetric_speed = 0.;
	    if (! mm3_per_mm.empty()) {
	        // In order to honor max_print_speed we need to find a target volumetric
	        // speed that we can use throughout the print. So we define this target 
	        // volumetric speed as the volumetric speed produced by printing the 
	        // smallest cross-section at the maximum speed: any larger cross-section
	        // will need slower feedrates.
            double max_print_speed = print.config().get_computed_value("max_print_speed");
            volumetric_speed = *std::min_element(mm3_per_mm.begin(), mm3_per_mm.end()) * max_print_speed;
	        // limit such volumetric speed with max_volumetric_speed if set
	        if (print.config().max_volumetric_speed.value > 0)
	            volumetric_speed = std::min(volumetric_speed, print.config().max_volumetric_speed.value);
	    }
	    return volumetric_speed;
	}


	static void init_ooze_prevention(const Print &print, OozePrevention &ooze_prevention)
	{
	    ooze_prevention.enable = print.config().ooze_prevention.value && ! print.config().single_extruder_multi_material;
	}

	// Fill in print_statistics and return formatted string containing filament statistics to be inserted into G-code comment section.
	static std::string update_print_stats_and_format_filament_stats(
	    const bool                   has_wipe_tower,
	    const WipeTowerData         &wipe_tower_data,
        const FullPrintConfig       &config,
	    const std::vector<Extruder> &extruders,
        unsigned int                 initial_extruder_id,
        int                          total_toolchanges,
        PrintStatistics              &print_statistics,
        bool                         export_binary_data,
        bgcode::binarize::BinaryData &binary_data)
	{
        std::string filament_stats_string_out;

        print_statistics.clear();
        print_statistics.total_toolchanges = total_toolchanges;
        print_statistics.initial_extruder_id = initial_extruder_id;
        std::vector<std::string> filament_types;
        if (! extruders.empty()) {
            std::pair<std::string, unsigned int> out_filament_used_mm(PrintStatistics::FilamentUsedMmMask + " ", 0);
            std::pair<std::string, unsigned int> out_filament_used_cm3(PrintStatistics::FilamentUsedCm3Mask + " ", 0);
            std::pair<std::string, unsigned int> out_filament_used_g(PrintStatistics::FilamentUsedGMask + " ", 0);
            std::pair<std::string, unsigned int> out_filament_cost(PrintStatistics::FilamentCostMask + " ", 0);
            for (const Extruder &extruder : extruders) {
                print_statistics.printing_extruders.emplace_back(extruder.id());
                filament_types.emplace_back(config.filament_type.get_at(extruder.id()));

                double used_filament   = extruder.used_filament() + (has_wipe_tower ? wipe_tower_data.used_filament_until_layer.back().second[extruder.id()] : 0.f);
                double extruded_volume = extruder.extruded_volume() + (has_wipe_tower ? wipe_tower_data.used_filament_until_layer.back().second[extruder.id()] * extruder.filament_crossection() : 0.f); // assumes 1.75mm filament diameter
                double filament_weight = extruded_volume * extruder.filament_density() * 0.001;
                double filament_cost   = filament_weight * extruder.filament_cost()    * 0.001;
                auto append = [&extruder](std::pair<std::string, unsigned int> &dst, const char *tmpl, double value) {
                    assert(is_decimal_separator_point());
                    while (dst.second < extruder.id()) {
                        // Fill in the non-printing extruders with zeros.
                        dst.first += (dst.second > 0) ? ", 0" : "0";
                        ++ dst.second;
                    }
                    if (dst.second > 0)
                        dst.first += ", ";
                    char buf[64];
                    sprintf(buf, tmpl, value);
                    dst.first += buf;
                    ++ dst.second;
                };
                if (!export_binary_data) {
                    append(out_filament_used_mm,  "%.2lf", used_filament);
                    append(out_filament_used_cm3, "%.2lf", extruded_volume * 0.001);
                }
                if (filament_weight > 0.) {
                    print_statistics.total_weight = print_statistics.total_weight + filament_weight;
                    if (!export_binary_data)
                        append(out_filament_used_g, "%.2lf", filament_weight);
                    if (filament_cost > 0.) {
                        print_statistics.total_cost = print_statistics.total_cost + filament_cost;
                        if (!export_binary_data)
                            append(out_filament_cost, "%.2lf", filament_cost);
                    }
                }
                print_statistics.total_used_filament += used_filament;
                print_statistics.total_extruded_volume += extruded_volume;
                print_statistics.total_wipe_tower_filament += has_wipe_tower ? used_filament - extruder.used_filament() : 0.;
                print_statistics.total_wipe_tower_filament_weight += has_wipe_tower ? (extruded_volume - extruder.extruded_volume()) * extruder.filament_density() * 0.001 : 0.;
                print_statistics.total_wipe_tower_cost += has_wipe_tower ? (extruded_volume - extruder.extruded_volume())* extruder.filament_density() * 0.001 * extruder.filament_cost() * 0.001 : 0.;
            }

            if (!export_binary_data) {
                filament_stats_string_out += out_filament_used_mm.first;
                filament_stats_string_out += "\n" + out_filament_used_cm3.first;
                if (out_filament_used_g.second)
                    filament_stats_string_out += "\n" + out_filament_used_g.first;
                if (out_filament_cost.second)
                    filament_stats_string_out += "\n" + out_filament_cost.first;
            }
            print_statistics.initial_filament_type = config.filament_type.get_at(initial_extruder_id);
            std::sort(filament_types.begin(), filament_types.end());
            print_statistics.printing_filament_types = filament_types.front();
            for (size_t i = 1; i < filament_types.size(); ++ i) {
                print_statistics.printing_filament_types += ",";
                print_statistics.printing_filament_types += filament_types[i];
            }
        }
        return filament_stats_string_out;
    }
}


// Sort the PrintObjects by their increasing Z, likely useful for avoiding colisions on Deltas during sequential prints.
static inline std::vector<const PrintInstance*> sort_object_instances_by_max_z(const Print& print)
{
    std::vector<const PrintObject*> objects(print.objects().begin(), print.objects().end());
    std::sort(objects.begin(), objects.end(), [](const PrintObject* po1, const PrintObject* po2) { return po1->height() < po2->height(); });
    std::vector<const PrintInstance*> instances;
    instances.reserve(objects.size());
    for (const PrintObject* object : objects)
        for (size_t i = 0; i < object->instances().size(); ++i)
            instances.emplace_back(&object->instances()[i]);
    return instances;
}


// Sort the PrintObjects by their increasing Y, likely useful for avoiding colisions on printer with a x-bar during sequential prints.
static inline std::vector<const PrintInstance*> sort_object_instances_by_max_y(const Print& print)
{
    std::vector<const PrintObject*> objects(print.objects().begin(), print.objects().end());
    std::sort(objects.begin(), objects.end(), [](const PrintObject* po1, const PrintObject* po2) { return po1->height() < po2->height(); });
    std::vector<const PrintInstance*> instances;
    instances.reserve(objects.size());
    std::map<const PrintInstance*, coord_t> map_min_y;
    for (const PrintObject* object : objects) {
        for (size_t i = 0; i < object->instances().size(); ++i) {
            instances.emplace_back(&object->instances()[i]);
            // Calculate the convex hull of a printable object. 
            Polygon poly = object->model_object()->convex_hull_2d(
                object->trafo()
                // already in object->trafo()
                //* Geometry::assemble_transform(Vec3d::Zero(),
                //    object->instances()[i].model_instance->get_rotation(), 
                //    object->instances()[i].model_instance->get_scaling_factor(), 
                //    object->instances()[i].model_instance->get_mirror())
            );
            BoundingBox bb(poly.points);
            Vec2crd offset = object->instances()[i].shift - object->center_offset();
            bb.translate(offset.x(), offset.y());
            map_min_y[instances.back()] = bb.min.y();
        }
    }
    std::sort(instances.begin(), instances.end(), [&map_min_y](const PrintInstance* po1, const PrintInstance* po2) { return map_min_y[po1] < map_min_y[po2]; });
    return instances;
}

// Produce a vector of PrintObjects in the order of their respective ModelObjects in print.model().
std::vector<const PrintInstance*> sort_object_instances_by_model_order(const Print& print)
{
    // Build up map from ModelInstance* to PrintInstance*
    std::vector<std::pair<const ModelInstance*, const PrintInstance*>> model_instance_to_print_instance;
    model_instance_to_print_instance.reserve(print.num_object_instances());
    for (const PrintObject *print_object : print.objects())
        for (const PrintInstance &print_instance : print_object->instances())
            model_instance_to_print_instance.emplace_back(print_instance.model_instance, &print_instance);
    std::sort(model_instance_to_print_instance.begin(), model_instance_to_print_instance.end(), [](auto &l, auto &r) { return l.first < r.first; });

    std::vector<const PrintInstance*> instances;
    instances.reserve(model_instance_to_print_instance.size());
    for (const ModelObject *model_object : print.model().objects)
        for (const ModelInstance *model_instance : model_object->instances) {
            auto it = std::lower_bound(model_instance_to_print_instance.begin(), model_instance_to_print_instance.end(), std::make_pair(model_instance, nullptr), [](auto &l, auto &r) { return l.first < r.first; });
            if (it != model_instance_to_print_instance.end() && it->first == model_instance)
                instances.emplace_back(it->second);
        }
    return instances;
}

// set standby temp for extruders
// Parse the custom G-code, try to find T, and add it if not present
void GCodeGenerator::_init_multiextruders(const Print& print, std::string& out, GCodeWriter & writer, const ToolOrdering &tool_ordering, const std::string &custom_gcode )
{

    //set standby temp for reprap
    if (std::set<uint8_t>{gcfRepRap}.count(print.config().gcode_flavor.value) > 0) {
        for (uint16_t tool_id : tool_ordering.all_extruders()) {
            int standby_temp = int(print.config().temperature.get_at(tool_id));
            if (standby_temp > 0) {
                if (print.config().ooze_prevention.value)
                    standby_temp += print.config().standby_temperature_delta.value;
                out.append("G10 P").append(std::to_string(tool_id)).append(" R").append(std::to_string(standby_temp)).append(" ; sets the standby temperature\n");
            }
        }
    }
}

struct LockMonitor
{
    PrintStatistics &m_status_monitor;
    LockMonitor(PrintStatistics &status_monitor) : m_status_monitor(status_monitor) { m_status_monitor.is_computing_gcode = true; }
    ~LockMonitor(){ m_status_monitor.is_computing_gcode = false; }
};

static inline bool arc_welder_enabled(const PrintConfig& print_config)
{
    return
        // Enabled
        print_config.arc_fitting != ArcFittingType::Disabled &&
        // Not a spiral vase print
        !print_config.spiral_vase &&
        // Presure equalizer not used
        print_config.max_volumetric_extrusion_rate_slope_negative == 0. &&
        print_config.max_volumetric_extrusion_rate_slope_positive == 0.;
}

void GCodeGenerator::_do_export(Print& print_mod, GCodeOutputStream &file, ThumbnailsGeneratorCallback thumbnail_cb)
{

    const Print &print = print_mod;
    Print::StatusMonitor status_monitor{print_mod};
    LockMonitor monitor_soft_lock(status_monitor.stats());
    this->m_throw_if_canceled =
        [&print]() { print.throw_if_canceled(); };

    const bool export_to_binary_gcode = print.full_print_config().option("binary_gcode")->get_bool();
    // if exporting gcode in binary format: 
    // we generate here the data to be passed to the post-processor, who is responsible to export them to file 
    // 1) generate the thumbnails
    // 2) collect the config data
    if (export_to_binary_gcode) {
        bgcode::binarize::BinaryData& binary_data = m_processor.get_binary_data();

        // Unit tests or command line slicing may not define "thumbnails" or "thumbnails_format".
        // If "thumbnails_format" is not defined, export to PNG.
        auto [thumbnails, errors] = GCodeThumbnails::make_and_check_thumbnail_list(print.full_print_config());

        if (errors != enum_bitmask<ThumbnailError>()) {
            std::string error_str = format("Invalid thumbnails value:");
            error_str += GCodeThumbnails::get_error_string(errors);
            throw Slic3r::ExportError(error_str);
        }

        if (!thumbnails.empty())
            GCodeThumbnails::generate_binary_thumbnails(
                thumbnail_cb, binary_data.thumbnails, thumbnails,
                [&print]() { print.throw_if_canceled(); });

        // file data
        binary_data.file_metadata.raw_data.emplace_back("Producer", std::string(SLIC3R_APP_NAME) + " " + std::string(SLIC3R_VERSION));

        // config data
        encode_full_config(print, binary_data.slicer_metadata.raw_data);

        // printer data - this section contains duplicates from the slicer metadata
        // that we just created. Find and copy the entries that we want to duplicate.
        const auto& slicer_metadata = binary_data.slicer_metadata.raw_data;
        const std::vector<std::string> keys_to_duplicate = { "printer_model", "filament_type", "nozzle_diameter", "bed_temperature",
                      "brim_width", "fill_density", "layer_height", "temperature", "ironing", "support_material", "extruder_colour" };
        assert(std::is_sorted(slicer_metadata.begin(), slicer_metadata.end(),
                              [](const auto& a, const auto& b) { return a.first < b.first; }));
        for (const std::string& key : keys_to_duplicate) {
            auto it = std::lower_bound(slicer_metadata.begin(), slicer_metadata.end(), std::make_pair(key, 0),
                [](const auto& a, const auto& b) { return a.first < b.first; });
            if (it != slicer_metadata.end() && it->first == key)
                binary_data.printer_metadata.raw_data.emplace_back(*it);
        }
    }

    //apply print config to m_config and m_writer, so we don't have to use print.config() instead
    // (and mostly to make m_writer.preamble() works)
    // this also reset the gcode writer
    this->apply_print_configs(print);
    this->m_wipe_tower_data = &print.wipe_tower_data();
    // modifies m_silent_time_estimator_enabled
    DoExport::init_gcode_processor(print.config(), m_processor, m_silent_time_estimator_enabled);

    //klipper can hide gcode into a macro, so add guessed init gcode to the processor.
    if (this->config().start_gcode_manual) {
        // from m_writer.preamble();
        m_processor.process_preamble(true/*unit_mm*/, true/*absolute_coords*/, m_writer.config.use_relative_e_distances.value, 0/*G92*/);
    }

    if (! print.config().gcode_substitutions.empty()) {
        m_find_replace = make_unique<GCodeFindReplace>(print.config());
        file.set_find_replace(m_find_replace.get(), false);
    }
    file.set_only_ascii(print.config().gcode_ascii.value);

    // resets analyzer's tracking data
    m_last_height  = 0.f;
    m_last_layer_z = 0.f;
    m_max_layer_z  = 0.f;
    m_last_width = 0.f;
#if ENABLE_GCODE_VIEWER_DATA_CHECKING
    m_last_mm3_per_mm = 0.;
#endif // ENABLE_GCODE_VIEWER_DATA_CHECKING
    m_fan_mover.release();

    status_monitor.stats().color_extruderid_to_used_filament.clear();
    status_monitor.stats().color_extruderid_to_used_weight.clear();
    status_monitor.stats().layer_area_stats.clear();

    // How many times will be change_layer() called?
    // change_layer() in turn increments the progress bar status.
    m_layer_count = 0;
    m_layer_with_support_count = 0;
    if (print.config().complete_objects.value) {
        // Add each of the object's layers separately.
        for (auto object : print.objects()) {
            std::vector<coordf_t> zs;
            std::vector<coordf_t> zs_with_supp;
            zs.reserve(object->layers().size());
            zs_with_supp.reserve(object->layers().size() + object->support_layers().size());
            for (auto layer : object->layers()) {
                zs.push_back(layer->print_z);
                zs_with_supp.push_back(layer->print_z);
            }
            for (auto layer : object->support_layers())
                zs_with_supp.push_back(layer->print_z);
            std::sort(zs.begin(), zs.end());
            std::sort(zs_with_supp.begin(), zs_with_supp.end());
            m_layer_with_support_count += (uint32_t)(object->instances().size()
                * (std::unique(zs_with_supp.begin(), zs_with_supp.end()) - zs_with_supp.begin()));
            m_layer_count += (uint32_t)(object->instances().size() * (std::unique(zs.begin(), zs.end()) - zs.begin()));
        }
    } else {
        // Print all objects with the same print_z together.
        std::vector<coordf_t> zs;
        std::vector<coordf_t> zs_with_supp;
        for (auto object : print.objects()) {
            zs.reserve(zs.size() + object->layers().size());
            zs_with_supp.reserve(zs.size() + object->layers().size() + object->support_layers().size());
            for (auto layer : object->layers()) {
                zs.push_back(layer->print_z);
                zs_with_supp.push_back(layer->print_z);
            }
            for (auto layer : object->support_layers())
                zs_with_supp.push_back(layer->print_z);
        }
        std::sort(zs.begin(), zs.end());
        std::sort(zs_with_supp.begin(), zs_with_supp.end());
        m_layer_count = (uint32_t)(std::unique(zs.begin(), zs.end()) - zs.begin());
        m_layer_with_support_count = (uint32_t)(std::unique(zs_with_supp.begin(), zs_with_supp.end()) - zs_with_supp.begin());
    }
     this->m_throw_if_canceled();

    //now that we have the layer count, init the status
    print.set_status(int(0), std::string(L("Generating G-code layer %s / %s")), std::vector<std::string>{ std::to_string(0), std::to_string(layer_count()) }, PrintBase::SlicingStatus::DEFAULT | PrintBase::SlicingStatus::SECONDARY_STATE);

    m_enable_cooling_markers = true;

    m_volumetric_speed = DoExport::autospeed_volumetric_limit(print);
     this->m_throw_if_canceled();

    if (print.config().spiral_vase.value)
        m_spiral_vase = make_unique<SpiralVase>(print.config());

    if (print.config().max_volumetric_extrusion_rate_slope_positive.value > 0 ||
        print.config().max_volumetric_extrusion_rate_slope_negative.value > 0)
        m_pressure_equalizer = make_unique<PressureEqualizer>(print.config());
    m_enable_extrusion_role_markers = (bool)m_pressure_equalizer;

    std::string preamble_to_put_start_layer = "";

    // if thumbnail type of BTT_TFT, insert above header
    // if not, it is inserted under the header in its normal spot
    const ConfigOptionEnum<GCodeThumbnailsFormat>* thumbnails_format = print.full_print_config().option<ConfigOptionEnum<GCodeThumbnailsFormat>>("thumbnails_format");
    const ConfigOptionBool* thumbnails_with_bed = print.full_print_config().option<ConfigOptionBool>("thumbnails_with_bed");
    if (!export_to_binary_gcode && thumbnails_format != nullptr && thumbnails_format->value == GCodeThumbnailsFormat::BIQU)
        GCodeThumbnails::export_thumbnails_to_file(thumbnail_cb,
            print.full_print_config().option<ConfigOptionPoints>("thumbnails")->get_values(),
            thumbnails_with_bed ? thumbnails_with_bed->value : false,
            thumbnails_format->value,
            true, 
            [&file](const char *sz) { file.write(sz); },
            this->m_throw_if_canceled);

    if (print.config().avoid_crossing_curled_overhangs){
        this->m_avoid_crossing_curled_overhangs.init_bed_shape(get_bed_shape(print.config()));
    }

    if (!export_to_binary_gcode)
        // Write information on the generator.
        file.write_format("; %s\n\n", Slic3r::header_slic3r_generated().c_str());

    
    const ConfigOptionBool* thumbnails_end_file = print.full_print_config().option<ConfigOptionBool>("thumbnails_end_file");
    if (! export_to_binary_gcode) {
    //print thumbnails at the start unless requested at the end.
        if(!thumbnails_end_file || !thumbnails_end_file->value) {
            const ConfigOptionBool* thumbnails_tag_with_format = print.full_print_config().option<ConfigOptionBool>("thumbnails_tag_format");
            // Unit tests or command line slicing may not define "thumbnails" or "thumbnails_format".
            // If "thumbnails_format" is not defined, export to PNG.
            GCodeThumbnails::export_thumbnails_to_file(thumbnail_cb,
                print.full_print_config().option<ConfigOptionPoints>("thumbnails")->get_values(),
                thumbnails_with_bed ? thumbnails_with_bed->value : false,
                thumbnails_format ? thumbnails_format->value : GCodeThumbnailsFormat::PNG,
                thumbnails_tag_with_format ? thumbnails_tag_with_format->value : false,
                [&file](const char* sz) { file.write(sz); },
                [&print]() { print.throw_if_canceled(); });
        }
    }

    // Write notes (content of the Print Settings tab -> Notes)
    {
        std::list<std::string> lines;
        boost::split(lines, print.config().notes.value, boost::is_any_of("\n"), boost::token_compress_off);
        for (auto line : lines) {
            // Remove the trailing '\r' from the '\r\n' sequence.
            if (! line.empty() && line.back() == '\r')
                line.pop_back();
            file.write_format("; %s\n", line.c_str());
        }
        if (! lines.empty())
            file.write("\n");
    }
     this->m_throw_if_canceled();

    // Write some terse information on the slicing parameters.
    const PrintObject *first_object         = print.objects().front();
    const double       layer_height         = first_object->config().layer_height.value;

    const double       first_layer_height   = print.get_first_layer_height();
    if (!export_to_binary_gcode) {
        for (size_t region_id = 0; region_id < print.num_print_regions(); ++ region_id) {
            const PrintRegion &region = print.get_print_region(region_id);
            file.write_format("; external perimeters extrusion width = %.2fmm\n", region.flow(*first_object, frExternalPerimeter, layer_height, 0).width());
            file.write_format("; perimeters extrusion width = %.2fmm\n",          region.flow(*first_object, frPerimeter,         layer_height, 0).width());
            file.write_format("; infill extrusion width = %.2fmm\n",              region.flow(*first_object, frInfill,            layer_height, 0).width());
            file.write_format("; solid infill extrusion width = %.2fmm\n",        region.flow(*first_object, frSolidInfill,       layer_height, 0).width());
            file.write_format("; top infill extrusion width = %.2fmm\n",          region.flow(*first_object, frTopSolidInfill,    layer_height, 0).width());
            //TODO add others
            if (print.has_support_material()) {
                file.write_format("; support material extrusion width = %.2fmm\n", support_material_flow(first_object).width());
                file.write_format("; support material interface extrusion width = %.2fmm\n", support_material_interface_flow(first_object).width());
            }
            if (first_object->config().first_layer_extrusion_width.value > 0)
                file.write_format("; first layer extrusion width = %.2fmm\n",   region.flow(*first_object, frPerimeter, first_layer_height, 0).width());
            file.write_format("\n");
        }
    }

     this->m_throw_if_canceled();

    // adds tags for time estimators
    if (print.config().remaining_times.value)
        file.write_format(";%s\n", GCodeProcessor::reserved_tag(GCodeProcessor::ETags::First_Line_M73_Placeholder).c_str());

    // Starting now, the G-code find / replace post-processor will be enabled.
    file.find_replace_enable();

    // Prepare the helper object for replacing placeholders in custom G-code and output filename.
    this->m_placeholder_parser_integration.parser = print.placeholder_parser();
    this->m_placeholder_parser_integration.parser.update_timestamp();
    this->m_placeholder_parser_integration.context.rng = std::mt19937(std::chrono::high_resolution_clock::now().time_since_epoch().count());
    // Enable passing global variables between PlaceholderParser invocations.
    this->m_placeholder_parser_integration.context.global_config = std::make_unique<DynamicConfig>();
    print.update_object_placeholders(this->m_placeholder_parser_integration.parser.config_writable(), ".gcode");

    // Get optimal tool ordering to minimize tool switches of a multi-exruder print.
    // For a print by objects, find the 1st printing object.
    ToolOrdering tool_ordering;
    uint16_t initial_extruder_id     = (uint16_t)-1;
    uint16_t final_extruder_id       = (uint16_t)-1;
    bool         has_wipe_tower      = false;
    std::vector<const PrintInstance*> 					print_object_instances_ordering;
    std::vector<const PrintInstance*>::const_iterator 	print_object_instance_sequential_active;
    bool has_milling = false;
    if (!config().milling_diameter.empty()) {
        for (const PrintObject* obj : print.objects()) {
            for (const Layer *layer : obj->layers()) {
                for (const LayerRegion *lr : layer->regions()) {
                    if (!lr->millings().empty()) {
                        has_milling = true;
                        break;
                    }
                }
            }
        }
    }
    if (print.config().complete_objects.value || print.config().parallel_objects_step.value > 0) {
        // Order object instances for sequential print.
        if(print.config().complete_objects_sort.value == cosObject)
            print_object_instances_ordering = sort_object_instances_by_model_order(print);
        else if (print.config().complete_objects_sort.value == cosZ)
            print_object_instances_ordering = sort_object_instances_by_max_z(print);
        else if (print.config().complete_objects_sort.value == cosY)
            print_object_instances_ordering = sort_object_instances_by_max_y(print);
        // Find the 1st printing object, find its tool ordering and the initial extruder ID.
        print_object_instance_sequential_active = print_object_instances_ordering.begin();
        for (; print_object_instance_sequential_active != print_object_instances_ordering.end(); ++ print_object_instance_sequential_active) {
            tool_ordering = ToolOrdering(*(*print_object_instance_sequential_active)->print_object, initial_extruder_id);
            if ((initial_extruder_id = tool_ordering.first_extruder()) != static_cast<uint16_t>(-1))
                break;
        }
        if (initial_extruder_id == static_cast<unsigned int>(-1))
            // No object to print was found, cancel the G-code export.
            throw Slic3r::SlicingError(_u8L("No extrusions were generated for objects."));
        // We don't allow switching of extruders per layer by Model::custom_gcode_per_print_z in sequential mode.
        // Use the extruder IDs collected from Regions.
        std::set<uint16_t> extruder_set = print.extruders();
    	this->set_extruders(std::vector<uint16_t>(extruder_set.begin(), extruder_set.end()));
        if(has_milling)
            m_writer.set_mills(std::vector<uint16_t>() = { 0 });
    } else {
        // Find tool ordering for all the objects at once, and the initial extruder ID.
        // If the tool ordering has been pre-calculated by Print class for wipe tower already, reuse it.
		tool_ordering = print.tool_ordering();
		tool_ordering.assign_custom_gcodes(print);
        if (tool_ordering.all_extruders().empty())
            // No object to print was found, cancel the G-code export.
            throw Slic3r::SlicingError(_u8L("No extrusions were generated for objects."));
        has_wipe_tower = print.has_wipe_tower() && tool_ordering.has_wipe_tower();
        initial_extruder_id = (has_wipe_tower && ! print.config().single_extruder_multi_material_priming) ?
            // The priming towers will be skipped.
            tool_ordering.all_extruders().back() :
            // Don't skip the priming towers.
            tool_ordering.first_extruder();
        // In non-sequential print, the printing extruders may have been modified by the extruder switches stored in Model::custom_gcode_per_print_z.
        // Therefore initialize the printing extruders from there.
    	this->set_extruders(tool_ordering.all_extruders());
        if (has_milling)
            m_writer.set_mills(std::vector<uint16_t>() = { 0 });
        // Order object instances using a nearest neighbor search.
        print_object_instances_ordering = chain_print_object_instances(print);
        // prusaslicer replaced the previous m_layer_count set by `m_layer_count=tool_ordering.layer_tools().size()` here
        assert(object_layer_count() == tool_ordering.layer_tools().size());
    }
    if (initial_extruder_id == (uint16_t)-1) {
        // Nothing to print!
        //initial_extruder_id = 0;
        //final_extruder_id   = 0;
    } else {
        final_extruder_id = tool_ordering.last_extruder();
        assert(final_extruder_id != (uint16_t)-1);
    }
     this->m_throw_if_canceled();

    m_cooling_buffer = make_unique<CoolingBuffer>(*this);
    m_cooling_buffer->set_current_extruder(initial_extruder_id);

    // Emit machine envelope limits for the Marlin firmware.
    this->print_machine_envelope(file, print);

    // Label all objects so printer knows about them since the start.
    m_label_objects.init(print);
    BoundingBoxf3 global_bounding_box;
    file.write(m_label_objects.all_objects_header(global_bounding_box, scale_d(print.config().resolution_internal.value)));
    
    // Update output variables after the extruders were initialized.
    this->m_placeholder_parser_integration.init(print.config(), m_writer);
    
    // Add variables from filament_custom_variables
    this->placeholder_parser().parse_custom_variables(m_config.print_custom_variables);
    this->placeholder_parser().parse_custom_variables(m_config.printer_custom_variables);
    this->placeholder_parser().parse_custom_variables(m_config.filament_custom_variables);

    // Add physical printer variables
    this->placeholder_parser().apply_config(print.physical_printer_config());

    // Let the start-up script prime the 1st printing tool.
    this->placeholder_parser().set("initial_tool", initial_extruder_id);
    this->placeholder_parser().set("initial_extruder", initial_extruder_id);
    this->placeholder_parser().set("current_extruder", initial_extruder_id);
    //Set variable for total layer count so it can be used in custom gcode.
    this->placeholder_parser().set("total_layer_count", object_layer_count());
    this->placeholder_parser().set("all_layer_count", layer_count());
    // Useful for sequential prints.
    this->placeholder_parser().set("current_object_idx", 0);
    // For the start / end G-code to do the priming and final filament pull in case there is no wipe tower provided.
    this->placeholder_parser().set("has_wipe_tower", has_wipe_tower);
    this->placeholder_parser().set("has_single_extruder_multi_material_priming", has_wipe_tower && print.config().single_extruder_multi_material_priming);
    this->placeholder_parser().set("total_toolchanges", tool_ordering.toolchanges_count());
    this->placeholder_parser().set("bounding_box", new ConfigOptionFloats({ global_bounding_box.min.x(), global_bounding_box.min.y(), global_bounding_box.min.z(), global_bounding_box.max.x(), global_bounding_box.max.y(), global_bounding_box.max.z() }));
    {
        BoundingBoxf bbox(print.config().bed_shape.get_values());
        assert(bbox.defined);
        if (! bbox.defined)
            // This should not happen, but let's make the compiler happy.
            bbox.min = bbox.max = Vec2d::Zero();
        this->placeholder_parser().set("print_bed_min",  new ConfigOptionFloats({ bbox.min.x(), bbox.min.y() }));
        this->placeholder_parser().set("print_bed_max",  new ConfigOptionFloats({ bbox.max.x(), bbox.max.y() }));
        this->placeholder_parser().set("print_bed_size", new ConfigOptionFloats({ bbox.size().x(), bbox.size().y() }));
    }
    if(!print.config().complete_objects.value){
        // Convex hull of the 1st layer extrusions, for bed leveling and placing the initial purge line.
        // It encompasses the object extrusions, support extrusions, skirt, brim, wipe tower.
        // It does NOT encompass user extrusions generated by custom G-code,
        // therefore it does NOT encompass the initial purge line.
        // It does NOT encompass MMU/MMU2 starting (wipe) areas.
        auto pts = std::make_unique<ConfigOptionPoints>();
        pts->resize(print.first_layer_convex_hull().size());
        for (size_t idx = 0; idx < print.first_layer_convex_hull().points.size(); ++idx)
            pts->set_at(unscale(print.first_layer_convex_hull().points[idx]), idx);
        BoundingBoxf bbox(pts->get_values());
        this->placeholder_parser().set("first_layer_print_convex_hull", pts.release());
        this->placeholder_parser().set("first_layer_print_min",  new ConfigOptionFloats({ bbox.min.x(), bbox.min.y() }));
        this->placeholder_parser().set("first_layer_print_max",  new ConfigOptionFloats({ bbox.max.x(), bbox.max.y() }));
        this->placeholder_parser().set("first_layer_print_size", new ConfigOptionFloats({ bbox.size().x(), bbox.size().y() }));
    } else {
        //have to compute it ourself :-/
        class BoundingBoxVisitor : public ExtrusionVisitorRecursiveConst {
        public:
            Point offset;
            Points hull;
            virtual void use(const ExtrusionPath& path) override {
                for (Point pt : path.polyline.to_polyline()) {
                    pt += offset;
                    hull.emplace_back(std::move(pt));
                }
            }
            virtual void use(const ExtrusionPath3D& path3D) override {
                for (Point pt : path3D.polyline.to_polyline()) {
                    pt += offset;
                    hull.emplace_back(std::move(pt));
                }
            }
            BoundingBoxf get_bb() {
                BoundingBox bbox(hull);
                return BoundingBoxf(unscaled(bbox.min), unscaled(bbox.max));
            }
            Polygon get_hull() { return Geometry::convex_hull(hull); }
        } bbvisitor;
        if (print.skirt_first_layer().has_value()) {
            print.skirt_first_layer()->visit(bbvisitor);
        } else if (print.skirt().entities().size() > 0) {
            print.skirt().visit(bbvisitor);
        } else {
            print.brim().visit(bbvisitor);
            for (const PrintObject* po : print.objects()) {
                for (const PrintInstance& inst : po->instances()) {
                    bbvisitor.offset = inst.shift;
                    if (po->skirt_first_layer().has_value()) {
                        po->skirt_first_layer()->visit(bbvisitor);
                    } else if (po->skirt().entities().size() > 0) {
                        po->skirt().visit(bbvisitor);
                    } else {
                        po->brim().visit(bbvisitor);
                        if (po->layers().empty()) continue;
                        const Layer* l = po->layers().front();
                        if (l->id() != 0) continue;
                        for (const LayerRegion* lr : l->regions()) {
                            lr->perimeters().visit(bbvisitor);
                            lr->fills().visit(bbvisitor);
                        }
                    }
                }
            }
        }
        BoundingBoxf bbox = bbvisitor.get_bb();
        Polygon first_layer_hull = bbvisitor.get_hull();
        auto pts = std::make_unique<ConfigOptionPoints>();
        pts->resize(first_layer_hull.size());
        for (size_t idx = 0; idx < first_layer_hull.points.size(); ++idx)
            pts->set_at(unscale(first_layer_hull.points[idx]), idx);
        this->placeholder_parser().set("first_layer_print_convex_hull", pts.release());
        this->placeholder_parser().set("first_layer_print_min", new ConfigOptionFloats({ bbox.min.x(), bbox.min.y() }));
        this->placeholder_parser().set("first_layer_print_max", new ConfigOptionFloats({ bbox.max.x(), bbox.max.y() }));
        this->placeholder_parser().set("first_layer_print_size", new ConfigOptionFloats({ bbox.size().x(), bbox.size().y() }));
    }
    {
        this->placeholder_parser().set("num_extruders", int(print.config().nozzle_diameter.size()));
        // PlaceholderParser currently substitues non-existent vector values with the zero'th value, which is harmful in the case of "is_extruder_used[]"
        // as Slicer may lie about availability of such non-existent extruder.
        // We rather sacrifice 256B of memory before we change the behavior of the PlaceholderParser, which should really only fill in the non-existent
        // vector elements for filament parameters.
        std::vector<unsigned char> is_extruder_used(std::max(size_t(255), print.config().nozzle_diameter.size()), 0);
        for (unsigned int extruder_id : tool_ordering.all_extruders())
            is_extruder_used[extruder_id] = true;
        this->placeholder_parser().set("is_extruder_used", new ConfigOptionBools(is_extruder_used));
    }

    //misc
    if (config().thumbnails_color.value.length() == 7) {
        this->placeholder_parser().set("thumbnails_color_int", new ConfigOptionInt((int)strtol(config().thumbnails_color.value.substr(1, 6).c_str(), NULL, 16)));
    }

    // Enable ooze prevention if configured so.
    DoExport::init_ooze_prevention(print, m_ooze_prevention);

    std::string start_gcode = this->placeholder_parser_process("start_gcode", print.config().start_gcode.value, initial_extruder_id);
    // get the start_filament_gcode to check if M109 or others are inside it
    std::string start_filament_gcode;
    if (!m_config.start_filament_gcode.get_at(initial_extruder_id).empty()) {
        DynamicConfig config;
        config.set_key_value("previous_extruder", new ConfigOptionInt(-1));
        config.set_key_value("next_extruder", new ConfigOptionInt((int)initial_extruder_id));
        config.set_key_value("layer_num", new ConfigOptionInt(0));
        config.set_key_value("layer_z", new ConfigOptionFloat(0)); //TODO: if the process is changed, please use the real first layer height
        start_filament_gcode = this->placeholder_parser_process("start_filament_gcode", m_config.start_filament_gcode.get_at(initial_extruder_id), initial_extruder_id, &config);
    }
    std::string start_all_gcode = start_gcode + "\"n" + start_filament_gcode;

    // Set chamber temperature
    if((initial_extruder_id != (uint16_t)-1) && !this->config().start_gcode_manual && print.config().chamber_temperature.get_at(initial_extruder_id) != 0)
         this->_print_first_layer_chamber_temperature(preamble_to_put_start_layer, print, start_all_gcode, initial_extruder_id, false);

    // Set bed temperature if the start G-code does not contain any bed temp control G-codes.
    if((initial_extruder_id != (uint16_t)-1) && !this->config().start_gcode_manual && this->config().gcode_flavor != gcfKlipper && print.config().first_layer_bed_temperature.get_at(initial_extruder_id) != 0)
         this->_print_first_layer_bed_temperature(preamble_to_put_start_layer, print, start_all_gcode, initial_extruder_id, false);

    //init extruders
    if (!this->config().start_gcode_manual)
        this->_init_multiextruders(print, preamble_to_put_start_layer, m_writer, tool_ordering, start_gcode);

    // Set extruder(s) temperature before and after start G-code.
    if ((initial_extruder_id != (uint16_t)-1) && !this->config().start_gcode_manual && (this->config().gcode_flavor != gcfKlipper || print.config().start_gcode.value.empty()) && print.config().first_layer_temperature.get_at(initial_extruder_id) != 0)
        this->_print_first_layer_extruder_temperatures(preamble_to_put_start_layer, print, start_all_gcode, initial_extruder_id, false);

    // adds tag for processor
    preamble_to_put_start_layer.append(";").append(GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Role)).append(gcode_extrusion_role_to_string(GCodeExtrusionRole::Custom)).append("\n");

    // Write the custom start G-code
    preamble_to_put_start_layer.append(start_gcode).append("\n");

    unset_last_pos();

    // Disable fan.
    if ((initial_extruder_id != (uint16_t) -1) && !this->config().start_gcode_manual && print.config().disable_fan_first_layers.get_at(initial_extruder_id)) {
        preamble_to_put_start_layer.append(m_writer.set_fan(uint8_t(0), initial_extruder_id));
    }

     this->m_throw_if_canceled();

    // Set other general things.
    preamble_to_put_start_layer.append(this->preamble());

    // Calculate wiping points if needed
    DoExport::init_ooze_prevention(print, m_ooze_prevention);
     this->m_throw_if_canceled();

    // Collect custom seam data from all objects.
     print.set_status(0, L("Computing seam visibility areas: object %s / %s"),
                      {"1", std::to_string(print.objects().size())},
                      PrintBase::SlicingStatus::FORCE_SHOW | PrintBase::SlicingStatus::SECONDARY_STATE);
    m_seam_placer.init(print, this->m_throw_if_canceled);

    //activate first extruder is multi-extruder and not in start-gcode
    if ((initial_extruder_id != (uint16_t)-1)) {
        if (m_writer.multiple_extruders) {
            //if not in gcode
            bool find = false;
            if (!start_gcode.empty()) {
                const char* ptr = start_gcode.data();
                while (*ptr != 0) {
                    // Skip whitespaces.
                    for (; *ptr == ' ' || *ptr == '\t'; ++ptr);
                    if (*ptr == 'T') {
                        // TX for most of the firmwares
                        find = true;
                        break;
                    } else if (*ptr == 'A' && print.config().gcode_flavor.value == gcfKlipper) {
                        // ACTIVATE_EXTRUDER for klipper (if used)
                        if (std::string::npos != start_gcode.find("ACTIVATE_EXTRUDER", size_t(ptr - start_gcode.data()))) {
                            find = true;
                            break;
                        }
                    }
                    // Skip the rest of the line.
                    for (; *ptr != 0 && *ptr != '\r' && *ptr != '\n'; ++ptr);
                    // Skip the end of line indicators.
                    for (; *ptr == '\r' || *ptr == '\n'; ++ptr);
                }
            }
            if (!find) {
                // Set initial extruder only after custom start G-code.
                // Ugly hack: Do not set the initial extruder if the extruder is primed using the MMU priming towers at the edge of the print bed.
                if (!(has_wipe_tower && print.config().single_extruder_multi_material_priming)) {
                    preamble_to_put_start_layer.append(this->set_extruder(initial_extruder_id, 0.));
                } else {
                    m_writer.toolchange(initial_extruder_id);
                }
            } else {
                // set writer to the tool as should be set in the start_gcode.
                preamble_to_put_start_layer.append(this->set_extruder(initial_extruder_id, 0., true));
            }
        } else {
            // if we are running a single-extruder setup, just set the extruder and "return nothing"
            preamble_to_put_start_layer.append(this->set_extruder(initial_extruder_id, 0.));
        }
    } else {
        // the right tool should have been set by the user.
        m_writer.toolchange(initial_extruder_id);
    }

    // ensure the first tool doesn't "extra_retract"
    m_writer.tool()->reset_retract();

    //write temps after custom gcodes to ensure the temperature are good. (after tool selection)
    if ((initial_extruder_id != (uint16_t)-1) && !this->config().start_gcode_manual && print.config().first_layer_temperature.get_at(initial_extruder_id) != 0)
        this->_print_first_layer_extruder_temperatures(preamble_to_put_start_layer, print, start_all_gcode, initial_extruder_id, true);
    if ((initial_extruder_id != (uint16_t)-1) && !this->config().start_gcode_manual && print.config().first_layer_bed_temperature.get_at(initial_extruder_id) != 0)
        this->_print_first_layer_bed_temperature(preamble_to_put_start_layer, print, start_all_gcode, initial_extruder_id, true);
    
    // Do all objects for each layer.
    if (initial_extruder_id != (uint16_t)-1) {
        if (print.config().complete_objects.value) {
            size_t finished_objects = 0;
            const PrintObject* prev_object = (*print_object_instance_sequential_active)->print_object;
            for (; print_object_instance_sequential_active != print_object_instances_ordering.end(); ++print_object_instance_sequential_active) {
                const PrintObject& object = *(*print_object_instance_sequential_active)->print_object;
                if (&object != prev_object || tool_ordering.first_extruder() != final_extruder_id) {
                    tool_ordering = ToolOrdering(object, final_extruder_id);
                    uint16_t new_extruder_id = tool_ordering.first_extruder();
                    if (new_extruder_id == (uint16_t)-1)
                        // Skip this object.
                        continue;
                    initial_extruder_id = new_extruder_id;
                    final_extruder_id = tool_ordering.last_extruder();
                    assert(final_extruder_id != (uint16_t)-1);
                }
                 this->m_throw_if_canceled();
                this->set_origin(unscale((*print_object_instance_sequential_active)->shift));
                if (finished_objects > 0) {
                    _move_to_print_object(preamble_to_put_start_layer, print, finished_objects, initial_extruder_id);
                    std::string between_objects_gcode =
                        this->placeholder_parser_process("between_objects_gcode",
                                                         print.config().between_objects_gcode.value,
                                                         initial_extruder_id);
                    // Set first layer bed and extruder temperatures, don't wait for it to reach the temperature.
                    this->_print_first_layer_bed_temperature(preamble_to_put_start_layer, print, between_objects_gcode, initial_extruder_id, false);
                    this->_print_first_layer_extruder_temperatures(preamble_to_put_start_layer, print, between_objects_gcode, initial_extruder_id, false);
                    preamble_to_put_start_layer.append(between_objects_gcode).append("\n");
                } else {
                    set_extra_lift(0, 0, print.config(), m_writer, initial_extruder_id);
                }
                //reinit the seam placer on the new object
                m_seam_placer.init(print, this->m_throw_if_canceled);
                // Reset the cooling buffer internal state (the current position, feed rate, accelerations).
                m_cooling_buffer->reset(this->writer().get_position());
                m_cooling_buffer->set_current_extruder(initial_extruder_id);
                // Process all layers of a single object instance (sequential mode) with a parallel pipeline:
                // Generate G-code, run the filters (vase mode, cooling buffer), run the G-code analyser
                // and export G-code into file.
                this->process_layers(print, status_monitor, tool_ordering, collect_layers_to_print(object, status_monitor),
                                     *print_object_instance_sequential_active - object.instances().data(),
                                     preamble_to_put_start_layer, file);
                ++finished_objects;
                // Flag indicating whether the nozzle temperature changes from 1st to 2nd layer were performed.
                // Reset it when starting another object from 1st layer.
                m_second_layer_things_done = false;
                prev_object = &object;
            }
            set_extra_lift(m_last_layer_z, prev_object->layers().back()->id(), print.config(), m_writer, initial_extruder_id /* osef, it's only for the lift_min */);
        } else {
            /////////////////////////////////////////////// begin parallel_objects_step
            if (print.config().parallel_objects_step > 0 && !has_wipe_tower) {
                double range = std::min(print.config().parallel_objects_step, print.config().extruder_clearance_height) + EPSILON;
                print_object_instances_ordering = chain_print_object_instances(print);
                bool first_layers = true;

                for (coordf_t Rstart = 0, Rend = range;; Rstart += range, Rend += range) {
                proceed_layers:
                    bool is_layers = false;
                    for (auto it_print_object_instance = print_object_instances_ordering.begin();
                         it_print_object_instance != print_object_instances_ordering.end();
                         ++it_print_object_instance) {
                        ObjectsLayerToPrint layers_to_print_range;
                        const PrintObject &       object        = *(*it_print_object_instance)->print_object;
                        ObjectsLayerToPrint object_layers = collect_layers_to_print(object, status_monitor);

                        for (const ObjectLayerToPrint &ltp : object_layers) {
                            if (ltp.print_z() < Rstart || ltp.print_z() >= Rend)
                                continue;

                            if (!first_layers && ltp.layer()->id() == 0)
                                continue;

                            layers_to_print_range.push_back(ltp);
                            if (first_layers)
                                break;
                        }

                        if (!layers_to_print_range.empty()) {
                            this->set_origin(unscale((*it_print_object_instance)->shift));

                            size_t finished_objects = 1 + (it_print_object_instance -
                                                           print_object_instances_ordering.begin());
                            if (finished_objects > 1)
                                _move_to_print_object(preamble_to_put_start_layer, print, finished_objects, initial_extruder_id);

                            assert(!object.instances().empty());
                            assert(*it_print_object_instance >= &*object.instances().begin() &&
                                   *it_print_object_instance <= &*(object.instances().end()-1));
                            this->process_layers(print, status_monitor, tool_ordering, layers_to_print_range,
                                                 *it_print_object_instance - object.instances().data(),
                                                 preamble_to_put_start_layer, file);
                            is_layers = true;
                        }
                    }
                    if (first_layers) {
                        first_layers = false;
                        goto proceed_layers;
                    }
                    if (!is_layers) {
                        break;
                    }
                }
                /////////////////////////////////////////////// end parallel_objects_step
            } else {
                // Sort layers by Z.
                // All extrusion moves with the same top layer height are extruded uninterrupted.
                std::vector<std::pair<coordf_t, ObjectsLayerToPrint>> layers_to_print = collect_layers_to_print(print, status_monitor);
                // Prusa Multi-Material wipe tower.
                if (has_wipe_tower && !layers_to_print.empty()) {
                    m_wipe_tower = std::make_unique<GCode::WipeTowerIntegration>(print.config(), *print.wipe_tower_data().priming.get(), print.wipe_tower_data().tool_changes, *print.wipe_tower_data().final_purge.get());

                    // Set position for wipe tower generation.
                    Vec3d new_position = this->writer().get_position();
                    new_position.z() = first_layer_height;
                    this->writer().update_position(new_position);

                    if (print.config().single_extruder_multi_material_priming) {
                    // TODO: 2.7: check that the preamble_to_put_start_layer has the z-move at first (from m_wipe_tower->prime, I guess)
                        preamble_to_put_start_layer.append(m_wipe_tower->prime(*this));
                        // Verify, whether the print overaps the priming extrusions.
                        BoundingBoxf bbox_print(get_print_extrusions_extents(print));
                        coordf_t twolayers_printz = ((layers_to_print.size() == 1) ? layers_to_print.front() : layers_to_print[1]).first + EPSILON;
                        for (const PrintObject* print_object : print.objects())
                            bbox_print.merge(get_print_object_extrusions_extents(*print_object, twolayers_printz));
                        bbox_print.merge(get_wipe_tower_extrusions_extents(print, twolayers_printz));
                        BoundingBoxf bbox_prime(get_wipe_tower_priming_extrusions_extents(print));
                        this->m_throw_if_canceled();
                        bbox_prime.offset(0.5f);
                        bool overlap = bbox_prime.overlap(bbox_print);

                        if (print.config().gcode_flavor.value == gcfMarlinLegacy || print.config().gcode_flavor.value == gcfMarlinFirmware) {
                            preamble_to_put_start_layer.append(this->retract_and_wipe());
                            preamble_to_put_start_layer.append("M300 S800 P500\n"); // Beep for 500ms, tone 800Hz.
                            if (overlap) {
                                // Wait for the user to remove the priming extrusions.
                                preamble_to_put_start_layer.append("M1 Remove priming towers and click button.\n");
                            } else {
                                // Just wait for a bit to let the user check, that the priming succeeded.
                                //TODO Add a message explaining what the printer is waiting for. This needs a firmware fix.
                                preamble_to_put_start_layer.append("M1 S10\n");
                            }
                        } else {
                            // This is not Marlin, M1 command is probably not supported.
                            // (See https://github.com/prusa3d/PrusaSlicer/issues/5441.)
                            if (overlap) {
                                status_monitor.active_step_add_warning(PrintStateBase::WarningLevel::CRITICAL,
                                    _u8L("Your print is very close to the priming regions. "
                                        "Make sure there is no collision."));
                            } else {
                                // Just continue printing, no action necessary.
                            }

                        }
                    }
                    this->m_throw_if_canceled();
                }
                // Process all layers of all objects (non-sequential mode) with a parallel pipeline:
                // Generate G-code, run the filters (vase mode, cooling buffer), run the G-code analyser
                // and export G-code into file.
                this->process_layers(print, status_monitor, tool_ordering, print_object_instances_ordering, layers_to_print, preamble_to_put_start_layer, file);
                if (m_wipe_tower)
                    // Purge the extruder, pull out the active filament.
                    file.write(m_wipe_tower->finalize(*this));
            }
        }
    }
    // Write end commands to file.
    file.write(this->retract_and_wipe());
    //if needed, write the gcode_label_objects_end
    {
        std::string gcode;
        _add_object_change_labels(gcode);
        file.write(gcode);
    }
    file.write(m_writer.set_fan(uint8_t(0)));

    // adds tag for processor
    file.write_format(";%s%s\n", GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Role).c_str(), gcode_extrusion_role_to_string(GCodeExtrusionRole::Custom).c_str());

    // Process filament-specific gcode in extruder order.
    if (initial_extruder_id != (uint16_t)-1) {
        uint16_t current_extruder_id = m_writer.tool()->id();
        DynamicConfig config;
        config.set_key_value("layer_num", new ConfigOptionInt(m_layer_index));
        config.set_key_value("layer_z", new ConfigOptionFloat(m_writer.get_position().z() - m_config.z_offset.value));
        config.set_key_value("max_layer_z", new ConfigOptionFloat(m_max_layer_z));
        config.set_key_value("filament_extruder_id", new ConfigOptionInt(current_extruder_id));
        config.set_key_value("previous_extruder", new ConfigOptionInt(current_extruder_id));
        config.set_key_value("next_extruder", new ConfigOptionInt(-1));
        if (print.config().single_extruder_multi_material) {
            if (m_writer.tool_is_extruder()) {
                // Process the end_filament_gcode for the active filament only.
                file.writeln(this->placeholder_parser_process("end_filament_gcode",
                                                              print.config().end_filament_gcode.get_at(current_extruder_id),
                                                              current_extruder_id, &config));
            }
        } else {
            //for all extruder used in this print
            for (uint16_t extruder_id : print.tool_ordering().all_extruders()) {
                //only for extruders
                auto extr_ids = m_writer.extruder_ids();
                if (std::find(extr_ids.begin(), extr_ids.end(), extruder_id) != extr_ids.end() ) {
                    //write end flament gcode.
                    const std::string& end_gcode = print.config().end_filament_gcode.get_at(extruder_id);
                    config.set_key_value("filament_extruder_id", new ConfigOptionInt(extruder_id));
                    config.set_key_value("previous_extruder", new ConfigOptionInt(current_extruder_id));
                    file.writeln(this->placeholder_parser_process("end_filament_gcode", end_gcode, extruder_id, &config));
                }
            }
        }
        file.writeln(this->placeholder_parser_process("end_gcode", print.config().end_gcode, m_writer.tool()->id(), &config));
    } else {
        assert(false); // what is the use-case?
    }
    file.write(m_writer.update_progress(layer_count(), layer_count(), true)); // 100%
    file.write(m_writer.postamble());

    // From now to the end of G-code, the G-code find / replace post-processor will be disabled.
    // Thus the PrusaSlicer generated config will NOT be processed by the G-code post-processor, see GH issue #7952.
    file.find_replace_supress();

    // adds tags for time estimators
    if (print.config().remaining_times.value)
        file.write_format(";%s\n", GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Last_Line_M73_Placeholder).c_str());

     this->m_throw_if_canceled();

    // Get filament stats.
    const std::string filament_stats_string_out = DoExport::update_print_stats_and_format_filament_stats(
        // Const inputs
        has_wipe_tower, print.wipe_tower_data(),
        this->config(),
        m_writer.extruders(),
        initial_extruder_id,
        tool_ordering.toolchanges_count(),
        // Modifies
        status_monitor.stats(),
        export_to_binary_gcode,
        m_processor.get_binary_data()
    );
    if (!export_to_binary_gcode)
        file.write(filament_stats_string_out);
    
    if (export_to_binary_gcode) {
        bgcode::binarize::BinaryData& binary_data = m_processor.get_binary_data();
        if (status_monitor.stats().total_toolchanges > 0)
            binary_data.print_metadata.raw_data.emplace_back("total toolchanges", std::to_string(status_monitor.stats().total_toolchanges));
        char buf[1024];
        sprintf(buf, "%.2lf", m_max_layer_z);
        binary_data.printer_metadata.raw_data.emplace_back("max_layer_z", buf);
    }
    else {
        // if exporting gcode in ascii format, statistics export is done here
        file.write("\n");
        file.write_format(PrintStatistics::TotalFilamentUsedGValueMask.c_str(), status_monitor.stats().total_weight);
        file.write_format(PrintStatistics::TotalFilamentCostValueMask.c_str(), status_monitor.stats().total_cost);
        file.write_format(PrintStatistics::TotalFilamentUsedWipeTowerValueMask.c_str(), status_monitor.stats().total_wipe_tower_filament_weight);
        if (status_monitor.stats().total_toolchanges > 0)
            file.write_format("; total toolchanges = %i\n", status_monitor.stats().total_toolchanges);
        file.write_format("; objects layers count = %i\n", object_layer_count());
        file.write_format("; total layers count = %i\n", layer_count());
        file.write_format(";%s\n", GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Estimated_Printing_Time_Placeholder).c_str());

        // if exporting gcode in ascii format, config export is done here
        // Append full config, delimited by two 'phony' configuration keys slic3r_config = begin and slic3r_config = end.
        // The delimiters are structured as configuration key / value pairs to be parsable by older versions of PrusaSlicer G-code viewer.
        file.flush();
        {
            file.write("\n; " SLIC3R_APP_NAME "_config = begin\n");
            std::string full_config;
            append_full_config(print, full_config);
            if (!full_config.empty())
                file.write(full_config);
            file.write("; " SLIC3R_APP_NAME "_config = end\n");
        }
        print.throw_if_canceled();

        //print thumbnails at the end instead of the start if requested (unless BTT / biqu thumbnail)
        if (!export_to_binary_gcode && thumbnails_end_file && thumbnails_end_file->value && (thumbnails_format == nullptr || thumbnails_format->value != GCodeThumbnailsFormat::BIQU)) {
            const ConfigOptionBool* thumbnails_tag_with_format = print.full_print_config().option<ConfigOptionBool>("thumbnails_tag_format");
            // Unit tests or command line slicing may not define "thumbnails" or "thumbnails_format".
            // If "thumbnails_format" is not defined, export to PNG.
            GCodeThumbnails::export_thumbnails_to_file(thumbnail_cb, 
                print.full_print_config().option<ConfigOptionPoints>("thumbnails")->get_values(),
                thumbnails_with_bed ? thumbnails_with_bed->value : false,
                thumbnails_format ? thumbnails_format->value : GCodeThumbnailsFormat::PNG,
                thumbnails_tag_with_format ? thumbnails_tag_with_format->value: false,
                [&file](const char* sz) { file.write(sz); },
                this->m_throw_if_canceled);
        }
    }
     this->m_throw_if_canceled();
}

void GCodeGenerator::_move_to_print_object(std::string& gcode_out, const Print& print, size_t finished_objects, uint16_t initial_extruder_id)
{
    // Move to the origin position for the copy we're going to print.
    // This happens before Z goes down to layer 0 again, so that no collision happens hopefully.
    m_enable_cooling_markers = false; // we're not filtering these moves through CoolingBuffer
    m_avoid_crossing_perimeters.use_external_mp_once();
    set_extra_lift(m_last_layer_z, 0, print.config(), m_writer, initial_extruder_id);
    gcode_out.append(this->retract_and_wipe());
    //go to origin of the next object (it's 0,0 because we shifted the origin to it)
    Polyline polyline = this->travel_to(gcode_out, Point(0, 0), ExtrusionRole::Travel);
    this->write_travel_to(gcode_out, polyline, "move to origin position for next object");
    m_enable_cooling_markers = true;
    // Disable motion planner when traveling to first object point.
    m_avoid_crossing_perimeters.disable_once();
    // Ff we are printing the bottom layer of an object, and we have already finished
    // another one, set first layer temperatures. This happens before the Z move
    // is triggered, so machine has more time to reach such temperatures.
    this->placeholder_parser().set("current_object_idx", int(finished_objects));
}

// Process all layers of all objects (non-sequential mode) with a parallel pipeline:
// Generate G-code, run the filters (vase mode, cooling buffer), run the G-code analyser
// and export G-code into file.
void GCodeGenerator::process_layers(
    const Print                                                         &print,
    Print::StatusMonitor                                                &status_monitor,
    const ToolOrdering                                                  &tool_ordering,
    const std::vector<const PrintInstance*>                             &print_object_instances_ordering,
    const std::vector<std::pair<coordf_t, ObjectsLayerToPrint>>         &layers_to_print,
    std::string                                                         &preamble,
    GCodeOutputStream                                                   &output_stream)
{
    // The pipeline is variable: The vase mode filter is optional.
    size_t layer_to_print_idx = 0;
    //const GCode::SmoothPathCache::InterpolationParameters interpolation_params = interpolation_parameters(print.config());
    // already done at the end of print(), with a real //
    //const auto smooth_path_interpolator = tbb::make_filter<void, std::pair<size_t, GCode::SmoothPathCache>>(slic3r_tbb_filtermode::serial_in_order,
    //    [this, &print, &layers_to_print, &layer_to_print_idx, &interpolation_params](tbb::flow_control &fc) -> std::pair<size_t, GCode::SmoothPathCache> {
    //        if (layer_to_print_idx >= layers_to_print.size()) {
    //            if (layer_to_print_idx == layers_to_print.size() + (m_pressure_equalizer ? 1 : 0)) {
    //                fc.stop();
    //                return {};
    //            } else {
    //                // Pressure equalizer need insert empty input. Because it returns one layer back.
    //                // Insert NOP (no operation) layer;
    //                return { layer_to_print_idx ++, {} };
    //            }
    //        } else {
    //            CNumericLocalesSetter locales_setter;
    //            print.throw_if_canceled();
    //            size_t idx = layer_to_print_idx ++;
    //            GCode::SmoothPathCache smooth_path_cache;
    //            for (const ObjectLayerToPrint &l : layers_to_print[idx].second)
    //                GCodeGenerator::smooth_path_interpolate(l, interpolation_params, smooth_path_cache);
    //            return { idx, std::move(smooth_path_cache) };
    //        }
    //    });

     const auto layer_select = tbb::make_filter<void, size_t>(slic3r_tbb_filtermode::serial_in_order,
        [this, &print, &layers_to_print, &layer_to_print_idx](tbb::flow_control &fc) -> size_t {
            if (layer_to_print_idx >= layers_to_print.size()) {
                if (layer_to_print_idx == layers_to_print.size() + (m_pressure_equalizer ? 1 : 0)) {
                    fc.stop();
                    return 0;
                } else {
                    // Pressure equalizer need insert empty input. Because it returns one layer back.
                    // Insert NOP (no operation) layer;
                    return layer_to_print_idx ++;
                }
            } else {
                CNumericLocalesSetter locales_setter;
                print.throw_if_canceled();
                return layer_to_print_idx++;
            }
        });
    const auto generator = tbb::make_filter<size_t, LayerResult>(slic3r_tbb_filtermode::serial_in_order,
        [this, &print, &status_monitor, &tool_ordering, &print_object_instances_ordering, &layers_to_print, &preamble](
            size_t layer_to_print_idx) -> LayerResult {
            if (layer_to_print_idx == layers_to_print.size()) {
                // Pressure equalizer need insert empty input. Because it returns one layer back.
                // Insert NOP (no operation) layer;
                LayerResult result = LayerResult::make_nop_layer_result();
                result.gcode = preamble;
                preamble.clear();
                return result;
            } else {
                CNumericLocalesSetter locales_setter;
                const std::pair<coordf_t, ObjectsLayerToPrint> &layer = layers_to_print[layer_to_print_idx];
                const LayerTools& layer_tools = tool_ordering.tools_for_layer(layer.first);
                if (m_wipe_tower && layer_tools.has_wipe_tower)
                    m_wipe_tower->next_layer();
                 this->m_throw_if_canceled();
                LayerResult result = this->process_layer(print, status_monitor, layer.second, layer_tools,
                                                         &layer == &layers_to_print.back(),
                                                         &print_object_instances_ordering, size_t(-1));
                result.gcode = preamble + result.gcode;
                preamble.clear();
                return result;
            }
        });
    // The pipeline is variable: The vase mode filter is optional.
    const auto spiral_vase = tbb::make_filter<LayerResult, LayerResult>(slic3r_tbb_filtermode::serial_in_order,
        [this, spiral_vase = this->m_spiral_vase.get()](LayerResult in) -> LayerResult {
            if (in.nop_layer_result)
                return in;
            this->m_throw_if_canceled();
            CNumericLocalesSetter locales_setter;
            spiral_vase->enable(in.spiral_vase_enable);
            return LayerResult{ spiral_vase->process_layer(std::move(in.gcode)), in.layer_id, in.spiral_vase_enable, in.cooling_buffer_flush};
        });
    const auto pressure_equalizer = tbb::make_filter<LayerResult, LayerResult>(slic3r_tbb_filtermode::serial_in_order,
        [this, pressure_equalizer = this->m_pressure_equalizer.get()](LayerResult in) -> LayerResult {
            this->m_throw_if_canceled();
            CNumericLocalesSetter locales_setter;
            return pressure_equalizer->process_layer(std::move(in));
        });
    const auto cooling = tbb::make_filter<LayerResult, std::string>(slic3r_tbb_filtermode::serial_in_order,
        [this, cooling_buffer = this->m_cooling_buffer.get()](LayerResult in) -> std::string {
             if (in.nop_layer_result)
                return in.gcode;
             this->m_throw_if_canceled();
             CNumericLocalesSetter locales_setter;
             return cooling_buffer->process_layer(std::move(in.gcode), in.layer_id, in.cooling_buffer_flush);
        });
    const auto find_replace = tbb::make_filter<std::string, std::string>(slic3r_tbb_filtermode::serial_in_order,
        [this, find_replace = this->m_find_replace.get()](std::string s) -> std::string {
            CNumericLocalesSetter locales_setter;
            this->m_throw_if_canceled();
            return find_replace->process_layer(std::move(s));
        });
    const auto output = tbb::make_filter<std::string, void>(slic3r_tbb_filtermode::serial_in_order,
        [this, &output_stream](std::string s) {
            CNumericLocalesSetter locales_setter;
            this->m_throw_if_canceled();
            output_stream.write(s);
        });

    const auto fan_mover = tbb::make_filter<std::string, std::string>(slic3r_tbb_filtermode::serial_in_order,
            [this, &fan_mover = this->m_fan_mover, &config = this->config(), &writer = this->m_writer](std::string in)->std::string {
        CNumericLocalesSetter locales_setter;

        if (fan_mover.get() == nullptr)
            fan_mover.reset(new Slic3r::FanMover(
                writer,
                std::abs((float)config.fan_speedup_time.value),
                config.fan_speedup_time.value > 0,
                config.use_relative_e_distances.value,
                config.fan_speedup_overhangs.value,
                (float)config.fan_kickstart.value));
        //flush as it's a whole layer
        this->m_throw_if_canceled();
        return fan_mover->process_gcode(in, true);
    });

    tbb::filter<void, LayerResult> pipeline_to_layerresult = layer_select & generator;
    if (m_spiral_vase)
        pipeline_to_layerresult = pipeline_to_layerresult & spiral_vase;
    if (m_pressure_equalizer)
        pipeline_to_layerresult = pipeline_to_layerresult & pressure_equalizer;

    tbb::filter<LayerResult, std::string> pipeline_to_string = cooling & fan_mover;
    if (m_find_replace)
        pipeline_to_string = pipeline_to_string & find_replace;

    // It registers a handler that sets locales to "C" before any TBB thread starts participating in tbb::parallel_pipeline.
    // Handler is unregistered when the destructor is called.
    TBBLocalesSetter locales_setter;
    // The pipeline elements are joined using const references, thus no copying is performed.
    output_stream.find_replace_supress();
    tbb::parallel_pipeline(12, pipeline_to_layerresult & pipeline_to_string & output);
    output_stream.find_replace_enable();
}

// Process all layers of a single object instance (sequential mode) with a parallel pipeline:
// Generate G-code, run the filters (vase mode, cooling buffer), run the G-code analyser
// and export G-code into file.
void GCodeGenerator::process_layers(
    const Print                             &print,
    Print::StatusMonitor                    &status_monitor,
    const ToolOrdering                      &tool_ordering,
    ObjectsLayerToPrint                      layers_to_print,
    const size_t                             single_object_idx,
    std::string                             &preamble,
    GCodeOutputStream                       &output_stream)
{
    // The pipeline is variable: The vase mode filter is optional.
    size_t layer_to_print_idx = 0;
    // already done at the end of print(), with a real //
    //const GCode::SmoothPathCache::InterpolationParameters interpolation_params = interpolation_parameters(print.config());
    //const auto smooth_path_interpolator = tbb::make_filter<void, std::pair<size_t, GCode::SmoothPathCache>> (slic3r_tbb_filtermode::serial_in_order,
    //    [this, &print, &layers_to_print, &layer_to_print_idx, interpolation_params](tbb::flow_control &fc) -> std::pair<size_t, GCode::SmoothPathCache> {
    //        if (layer_to_print_idx >= layers_to_print.size()) {
    //            if (layer_to_print_idx == layers_to_print.size() + (m_pressure_equalizer ? 1 : 0)) {
    //                fc.stop();
    //                return {};
    //            } else {
    //                // Pressure equalizer need insert empty input. Because it returns one layer back.
    //                // Insert NOP (no operation) layer;
    //                return { layer_to_print_idx ++, {} };
    //            }
    //        } else {
    //            CNumericLocalesSetter locales_setter;
    //            print.throw_if_canceled();
    //            size_t idx = layer_to_print_idx ++;
    //            GCode::SmoothPathCache smooth_path_cache;
    //            GCodeGenerator::smooth_path_interpolate(layers_to_print[idx], interpolation_params, smooth_path_cache);
    //            return { idx, std::move(smooth_path_cache) };
    //        }
    //    });
     const auto layer_select = tbb::make_filter<void, size_t>(slic3r_tbb_filtermode::serial_in_order,
        [this, &print, &layers_to_print, &layer_to_print_idx](tbb::flow_control &fc) -> size_t {
            if (layer_to_print_idx >= layers_to_print.size()) {
                if (layer_to_print_idx == layers_to_print.size() + (m_pressure_equalizer ? 1 : 0)) {
                    fc.stop();
                    return 0;
                } else {
                    // Pressure equalizer need insert empty input. Because it returns one layer back.
                    // Insert NOP (no operation) layer;
                    return layer_to_print_idx ++;
                }
            } else {
                CNumericLocalesSetter locales_setter;
                 this->m_throw_if_canceled();
                return layer_to_print_idx++;
            }
        });
    const auto generator = tbb::make_filter<size_t, LayerResult>(slic3r_tbb_filtermode::serial_in_order,
        [this, &print, &status_monitor, &tool_ordering, &layers_to_print, single_object_idx, &preamble](size_t layer_to_print_idx) -> LayerResult {
            if (layer_to_print_idx == layers_to_print.size()) {
                // Pressure equalizer need insert empty input. Because it returns one layer back.
                // Insert NOP (no operation) layer;
                LayerResult result = LayerResult::make_nop_layer_result();
                result.gcode = preamble;
                preamble.clear();
                return result;
            } else {
                CNumericLocalesSetter locales_setter;
                ObjectLayerToPrint &layer = layers_to_print[layer_to_print_idx];
                print.throw_if_canceled();
                LayerResult result = this->process_layer(print, status_monitor, {std::move(layer)},
                                                         tool_ordering.tools_for_layer(layer.print_z()),
                                                         &layer == &layers_to_print.back(), nullptr,
                                                         single_object_idx);
                result.gcode = preamble + result.gcode;
                preamble.clear();
                return result;
            }
        });
    // The pipeline is variable: The vase mode filter is optional.
    const auto spiral_vase = tbb::make_filter<LayerResult, LayerResult>(slic3r_tbb_filtermode::serial_in_order,
        [this, spiral_vase = this->m_spiral_vase.get()](LayerResult in)->LayerResult {
            if (in.nop_layer_result)
                return in;
            this->m_throw_if_canceled();
            CNumericLocalesSetter locales_setter;
            spiral_vase->enable(in.spiral_vase_enable);
            return { spiral_vase->process_layer(std::move(in.gcode)), in.layer_id, in.spiral_vase_enable, in.cooling_buffer_flush };
        });
    const auto pressure_equalizer = tbb::make_filter<LayerResult, LayerResult>(slic3r_tbb_filtermode::serial_in_order,
        [this, pressure_equalizer = this->m_pressure_equalizer.get()](LayerResult in) -> LayerResult {
             this->m_throw_if_canceled();
             CNumericLocalesSetter locales_setter;
             return pressure_equalizer->process_layer(std::move(in));
        });
    const auto cooling = tbb::make_filter<LayerResult, std::string>(slic3r_tbb_filtermode::serial_in_order,
        [this, cooling_buffer = this->m_cooling_buffer.get()](LayerResult in)->std::string {
            if (in.nop_layer_result)
                return in.gcode;
            this->m_throw_if_canceled();            CNumericLocalesSetter locales_setter;
            return cooling_buffer->process_layer(std::move(in.gcode), in.layer_id, in.cooling_buffer_flush);
        });
    const auto find_replace = tbb::make_filter<std::string, std::string>(slic3r_tbb_filtermode::serial_in_order,
        [this, find_replace = this->m_find_replace.get()](std::string s) -> std::string {
            this->m_throw_if_canceled();
            CNumericLocalesSetter locales_setter;
            return find_replace->process_layer(std::move(s));
        });
    const auto output = tbb::make_filter<std::string, void>(slic3r_tbb_filtermode::serial_in_order,
        [this, &output_stream](std::string s) {
            this->m_throw_if_canceled();
            CNumericLocalesSetter locales_setter;
            output_stream.write(s);
        });

    const auto fan_mover = tbb::make_filter<std::string, std::string>(slic3r_tbb_filtermode::serial_in_order,
        [this, &fan_mover = this->m_fan_mover, &config = this->config(), &writer = this->m_writer](std::string in)->std::string {
        if (fan_mover.get() == nullptr)
            fan_mover.reset(new Slic3r::FanMover(
                writer,
                std::abs((float)config.fan_speedup_time.value),
                config.fan_speedup_time.value > 0,
                config.use_relative_e_distances.value,
                config.fan_speedup_overhangs.value,
                (float)config.fan_kickstart.value));
        this->m_throw_if_canceled();
        //flush as it's a whole layer
        return fan_mover->process_gcode(in, true);
    });

    tbb::filter<void, LayerResult> pipeline_to_layerresult = layer_select & generator;
    if (m_spiral_vase)
        pipeline_to_layerresult = pipeline_to_layerresult & spiral_vase;
    if (m_pressure_equalizer)
        pipeline_to_layerresult = pipeline_to_layerresult & pressure_equalizer;

    tbb::filter<LayerResult, std::string> pipeline_to_string = cooling & fan_mover;
    if (m_find_replace)
        pipeline_to_string = pipeline_to_string & find_replace;

    // It registers a handler that sets locales to "C" before any TBB thread starts participating in tbb::parallel_pipeline.
    // Handler is unregistered when the destructor is called.
    TBBLocalesSetter locales_setter;
    // The pipeline elements are joined using const references, thus no copying is performed.
    output_stream.find_replace_supress();
    tbb::parallel_pipeline(12, pipeline_to_layerresult & pipeline_to_string & output);
    output_stream.find_replace_enable();
}

std::string GCodeGenerator::placeholder_parser_process(
    const std::string   &name,
    const std::string   &templ,
    uint16_t             current_extruder_id,
    const DynamicConfig *config_override)
{
    if (current_extruder_id == uint16_t(-1)) {
        current_extruder_id = this->m_writer.tool()->id();
    }
#ifndef NDEBUG // CHECK_CUSTOM_GCODE_PLACEHOLDERS
    if (config_override) {
        const auto& custom_gcode_placeholders = custom_gcode_specific_placeholders();

        // 1-st check: custom G-code "name" have to be present in s_CustomGcodeSpecificOptions;
        //if (custom_gcode_placeholders.count(name) > 0) {
        //    const auto& placeholders = custom_gcode_placeholders.at(name);
        if (auto it = custom_gcode_placeholders.find(name); it != custom_gcode_placeholders.end()) {
            const auto& placeholders = it->second;

            for (const std::string& key : config_override->keys()) {
                // 2-nd check: "key" have to be present in s_CustomGcodeSpecificOptions for "name" custom G-code ;
                if (std::find(placeholders.begin(), placeholders.end(), key) == placeholders.end())
                    throw Slic3r::PlaceholderParserError(format("\"%s\" placeholder for \"%s\" custom G-code \n"
                                                                "needs to be added to s_CustomGcodeSpecificOptions", key.c_str(), name.c_str()));
                // 3-rd check: "key" have to be present in CustomGcodeSpecificConfigDef for "key" placeholder;
                if (!custom_gcode_specific_config_def.has(key))
                    throw Slic3r::PlaceholderParserError(format("Definition of \"%s\" placeholder \n"
                                                                "needs to be added to CustomGcodeSpecificConfigDef", key.c_str()));
            }
        }
        else
            throw Slic3r::PlaceholderParserError(format("\"%s\" custom G-code needs to be added to s_CustomGcodeSpecificOptions", name.c_str()));
    }
#endif

    PlaceholderParserIntegration &ppi = m_placeholder_parser_integration;
    try {
        //2.7: check where to add that (in PlaceholderParserIntegration ? )
        //add some config conversion for colors
        //auto func_add_colour = [&default_config](std::string key, std::string colour) {
        //    if (colour.length() == 7) {
        //        default_config.set_key_value(key, new ConfigOptionInt((int)strtol(colour.substr(1, 6).c_str(), NULL, 16)));
        //    }
        //};
        //if (current_extruder_id >= 0 && current_extruder_id < config().filament_colour.size()) {
        //    func_add_colour("filament_colour_int", config().filament_colour.get_at(current_extruder_id));
        //    func_add_colour("extruder_colour_int", config().extruder_colour.get_at(current_extruder_id));
        //}
        //// should be the same as Vec2d gcode_pos = point_to_gcode(m_last_pos);
        //Vec3d gcode_pos = this->writer().get_position();
        //default_config.set_key_value("current_position", new ConfigOptionFloats( {gcode_pos.x(), gcode_pos.y(), gcode_pos.z()} ));
        //default_config.set_key_value("current_object_position", new ConfigOptionFloats( {m_origin.x(), m_origin.y()} ));

        ppi.update_from_gcodewriter(m_writer, *this->m_wipe_tower_data);
        std::string output = ppi.parser.process(templ, current_extruder_id, config_override, &ppi.output_config, &ppi.context);
        ppi.validate_output_vector_variables();

        if (const std::vector<double> &pos = ppi.opt_position->get_values(); ppi.position != pos) {
            // Update G-code writer.
            m_writer.update_position({ pos[0], pos[1], pos[2] });
            this->set_last_pos(this->gcode_to_point({pos[0], pos[1]}));
        }

        for (const Extruder &e : m_writer.extruders()) {
            unsigned int eid = e.id();
            assert(eid < ppi.num_extruders);
            if ( eid < ppi.num_extruders) {
                if (! m_writer.config.use_relative_e_distances && ! is_approx(ppi.e_position[eid], ppi.opt_e_position->get_at(eid)))
                    const_cast<Extruder&>(e).set_position(ppi.opt_e_position->get_at(eid));
                if (! is_approx(ppi.e_retracted[eid], ppi.opt_e_retracted->get_at(eid)) || 
                    ! is_approx(ppi.e_restart_extra[eid], ppi.opt_e_restart_extra->get_at(eid)))
                    const_cast<Extruder&>(e).set_retracted(ppi.opt_e_retracted->get_at(eid), ppi.opt_e_restart_extra->get_at(eid));
            }
        }
        // add tag for fan_mover, to avoid to touch this section.
        if (!output.empty() && (m_config.gcode_comments || m_config.fan_speedup_time.value != 0 || m_config.fan_kickstart.value != 0 )) {
            output = "; custom gcode: " + name + "\n" + output;
            check_add_eol(output);
            output += "; custom gcode end: "+ name + "\n";
        }
        return output;
    } 
    catch (std::runtime_error &err) 
    {
        // Collect the names of failed template substitutions for ExtrusionRole::ror reporting.
        auto it = ppi.failed_templates.find(name);
        if (it == ppi.failed_templates.end())
            // Only if there was no ExtrusionRole::ror reported for this template, store the first ExtrusionRole::ror message into the map to be reported.
            // We don't want to collect ExtrusionRole::ror message for each and every occurence of a single custom G-code section.
            ppi.failed_templates.insert(it, std::make_pair(name, std::string(err.what())));
        // Insert the macro ExtrusionRole::ror message into the G-code.
        return
            std::string("\n!!!!! Failed to process the custom G-code template ") + name + "\n" +
            err.what() +
            "!!!!! End of an ExtrusionRole::ror report for the custom G-code template " + name + "\n\n";
    }
}

// Parse the custom G-code, try to find mcode_set_temp_dont_wait and mcode_set_temp_and_wait or optionally G10 with temperature inside the custom G-code.
// Returns true if one of the temp commands are found, and try to parse the target temperature value into temp_out.
static bool custom_gcode_sets_temperature(const std::string &gcode, const int mcode_set_temp_dont_wait, const int mcode_set_temp_and_wait, const bool include_g10, int &temp_out)
{
    temp_out = -1;
    if (gcode.empty())
        return false;

    const char *ptr = gcode.data();
    bool temp_set_by_gcode = false;
    while (*ptr != 0) {
        // Skip whitespaces.
        for (; *ptr == ' ' || *ptr == '\t'; ++ ptr);
        if (*ptr == 'M' || // Line starts with 'M'. It is a machine command.
            (*ptr == 'G' && include_g10)) { // Only check for G10 if requested
            bool is_gcode = *ptr == 'G';
            ++ ptr;
            // Parse the M or G code value.
            char *endptr = nullptr;
            int mgcode = int(strtol(ptr, &endptr, 10));
            if (endptr != nullptr && endptr != ptr && 
                (is_gcode ?
                    // G10 found
                    (mgcode == 10) :
                // M104/M109 or M140/M190 found.
                    (mgcode == mcode_set_temp_dont_wait || mgcode == mcode_set_temp_and_wait))) {
				ptr = endptr;
                if (! is_gcode)
                    // Let the caller know that the custom M-code sets the temperature.
                temp_set_by_gcode = true;
                // Now try to parse the temperature value.
				// While not at the end of the line:
				while (strchr(";\r\n\0", *ptr) == nullptr) {
                    // Skip whitespaces.
                    for (; *ptr == ' ' || *ptr == '\t'; ++ ptr);
                    if (*ptr == 'S') {
                        // Skip whitespaces.
                        for (++ ptr; *ptr == ' ' || *ptr == '\t'; ++ ptr);
                        // Parse an int.
                        endptr = nullptr;
                        long temp_parsed = strtol(ptr, &endptr, 10);
						if (endptr > ptr) {
							ptr = endptr;
							temp_out = temp_parsed;
                            // Let the caller know that the custom G-code sets the temperature
                            // Only do this after successfully parsing temperature since G10
                            // can be used for other reasons
                            temp_set_by_gcode = true;
						}
                    } else {
                        // Skip this word.
						for (; strchr(" \t;\r\n\0", *ptr) == nullptr; ++ ptr);
                    }
                }
            }
        }
        // Skip the rest of the line.
        for (; *ptr != 0 && *ptr != '\r' && *ptr != '\n'; ++ ptr);
		// Skip the end of line indicators.
        for (; *ptr == '\r' || *ptr == '\n'; ++ ptr);
	}
    return temp_set_by_gcode;
}

// Print the machine envelope G-code for the Marlin firmware based on the "machine_max_xxx" parameters.
// Do not process this piece of G-code by the time estimator, it already knows the values through another sources.
void GCodeGenerator::print_machine_envelope(GCodeOutputStream &file, const Print &print)
{
   // gcfRepRap, gcfRepetier, gcfTeacup, gcfMakerWare, gcfMarlinLegacy, gcfMarlinFirmware, gcfKlipper, gcfSailfish, gcfSprinter, gcfMach3, gcfMachinekit,
   ///     gcfSmoothie, gcfNoExtrusion,
    if (print.config().machine_limits_usage.value == MachineLimitsUsage::EmitToGCode) {
        // some firmware are using mm/sec and some others mm/min for M203 and M566
        int factor = (std::set<uint8_t>{gcfMarlinLegacy, gcfMarlinFirmware, gcfSmoothie}.count(print.config().gcode_flavor.value) > 0) ? 1 : 60;
        if (std::set<uint8_t>{gcfMarlinLegacy, gcfMarlinFirmware, gcfRepetier, gcfRepRap,  gcfSprinter}.count(print.config().gcode_flavor.value) > 0)
            file.write_format("M201 X%d Y%d Z%d E%d ; sets maximum accelerations, mm/sec^2\n",
                int(print.config().machine_max_acceleration_x.get_at(0) + 0.5),
                int(print.config().machine_max_acceleration_y.get_at(0) + 0.5),
                int(print.config().machine_max_acceleration_z.get_at(0) + 0.5),
                int(print.config().machine_max_acceleration_e.get_at(0) + 0.5));
        if (std::set<uint8_t>{gcfRepetier}.count(print.config().gcode_flavor.value) > 0)
            file.write_format("M202 X%d Y%d ; sets maximum travel acceleration\n",
                int(print.config().machine_max_acceleration_travel.get_at(0) + 0.5),
                int(print.config().machine_max_acceleration_travel.get_at(0) + 0.5));
        if (std::set<uint8_t>{gcfMarlinLegacy, gcfMarlinFirmware, gcfRepetier, gcfSmoothie, gcfSprinter}.count(print.config().gcode_flavor.value) > 0)
            file.write_format("M203 X%d Y%d Z%d E%d ; sets maximum feedrates, %s\n",
                int(print.config().machine_max_feedrate_x.get_at(0) * factor + 0.5),
                int(print.config().machine_max_feedrate_y.get_at(0) * factor + 0.5),
                int(print.config().machine_max_feedrate_z.get_at(0) * factor + 0.5),
                int(print.config().machine_max_feedrate_e.get_at(0) * factor + 0.5),
                factor == 60 ? "mm / min" : "mm / sec");
        if (print.config().gcode_flavor.value == gcfRepRap) {
            file.write_format("M203 X%d Y%d Z%d E%d I%d; sets maximum feedrates, mm/min\n",
                int(print.config().machine_max_feedrate_x.get_at(0) * factor + 0.5),
                int(print.config().machine_max_feedrate_y.get_at(0) * factor + 0.5),
                int(print.config().machine_max_feedrate_z.get_at(0) * factor + 0.5),
                int(print.config().machine_max_feedrate_e.get_at(0) * factor + 0.5),
                int(print.config().machine_min_extruding_rate.get_at(0) * factor + 0.5));
        }
        // Acceleration
        // Now M204 - acceleration. This one is quite hairy thanks to how Marlin guys care about
        // backwards compatibility: https://github.com/prusa3d/PrusaSlicer/issues/1089
        // Legacy Marlin should export travel acceleration the same as printing acceleration.
        // MarlinFirmware has the two separated.
        if (gcfMarlinLegacy == print.config().gcode_flavor)
            // Legacy Marlin uses M204 S[print] T[retract]
            file.write_format("M204 S%d T%d ; sets acceleration (S) and retract acceleration (R), mm/sec^2\n",
                int(print.config().machine_max_acceleration_extruding.get_at(0) + 0.5),
                int(print.config().machine_max_acceleration_retracting.get_at(0) + 0.5));
        else if (std::set<uint8_t>{gcfMarlinFirmware}.count(print.config().gcode_flavor.value) > 0)
            file.write_format("M204 P%d R%d T%d ; sets acceleration (P, T) and retract acceleration (R), mm/sec^2\n",
                int(print.config().machine_max_acceleration_extruding.get_at(0) + 0.5),
                int(print.config().machine_max_acceleration_retracting.get_at(0) + 0.5),
                int(print.config().machine_max_acceleration_travel.get_at(0) + 0.5));
        else if (std::set<uint8_t>{gcfRepRap, gcfKlipper, gcfSprinter}.count(print.config().gcode_flavor.value) > 0)
            // Uses M204 P[print] T[travel]
            file.write_format("M204 P%d T%d ; sets acceleration (P, T), mm/sec^2\n",
                int(print.config().machine_max_acceleration_extruding.get_at(0) + 0.5),
                int(print.config().machine_max_acceleration_travel.get_at(0) + 0.5));
        // jerk
        if (std::set<uint8_t>{gcfRepRap}.count(print.config().gcode_flavor.value) > 0)
            file.write_format("M566 X%.2lf Y%.2lf Z%.2lf E%.2lf ; sets the jerk limits, mm/min\n",
                print.config().machine_max_jerk_x.get_at(0) * factor,
                print.config().machine_max_jerk_y.get_at(0) * factor,
                print.config().machine_max_jerk_z.get_at(0) * factor,
                print.config().machine_max_jerk_e.get_at(0) * factor);
        else if (std::set<uint8_t>{gcfMarlinLegacy, gcfMarlinFirmware, gcfRepetier}.count(print.config().gcode_flavor.value) > 0)
            file.write_format("M205 X%.2lf Y%.2lf Z%.2lf E%.2lf ; sets the jerk limits, mm/sec\n",
                print.config().machine_max_jerk_x.get_at(0),
                print.config().machine_max_jerk_y.get_at(0),
                print.config().machine_max_jerk_z.get_at(0),
                print.config().machine_max_jerk_e.get_at(0));
        else if (std::set<uint8_t>{gcfSmoothie}.count(print.config().gcode_flavor.value) > 0)
            file.write_format("M205 X%.2lf Z%.2lf ; sets the jerk limits, mm/sec\n",
                std::min(print.config().machine_max_jerk_x.get_at(0),
                print.config().machine_max_jerk_y.get_at(0)),
                print.config().machine_max_jerk_z.get_at(0));
        // min feedrate
        if (std::set<uint8_t>{gcfMarlinLegacy, gcfMarlinFirmware, gcfRepetier}.count(print.config().gcode_flavor.value) > 0)
            file.write_format("M205 S%d T%d ; sets the minimum extruding and travel feed rate, mm/sec\n",
                int(print.config().machine_min_extruding_rate.get_at(0) + 0.5),
                int(print.config().machine_min_travel_rate.get_at(0) + 0.5));
    }
}

// Write 1st layer bed temperatures into the G-code.
// Only do that if the start G-code does not already contain any M-code controlling an extruder temperature.
// M140 - Set Bed Temperature
// M190 - Set Bed Temperature and Wait
void GCodeGenerator::_print_first_layer_bed_temperature(std::string &out, const Print &print, const std::string &gcode, uint16_t first_printing_extruder_id, bool wait)
{
    bool autoemit = print.config().autoemit_temperature_commands;
    // Initial bed temperature based on the first extruder.
    int  temp = print.config().first_layer_bed_temperature.get_at(first_printing_extruder_id);
    //disable bed temp control if 0
    if (temp == 0) return;
    // Is the bed temperature set by the provided custom G-code?
    int  temp_by_gcode     = -1;
    bool temp_set_by_gcode = custom_gcode_sets_temperature(gcode, 140, 190, false, temp_by_gcode);
    if (autoemit && temp_set_by_gcode && temp_by_gcode >= 0 && temp_by_gcode < 1000)
        temp = temp_by_gcode;
    // Always call m_writer.set_bed_temperature() so it will set the internal "current" state of the bed temp as if
    // the custom start G-code emited these.
    std::string set_temp_gcode = m_writer.set_bed_temperature(temp, wait);
    if (autoemit && !temp_set_by_gcode)
        out += (set_temp_gcode);
}

// Write 1st layer chamber temperatures into the G-code.
// Only do that if the start G-code does not already contain any M-code controlling an extruder temperature.
// M141 - Set chamber Temperature
// M191 - Set chamber Temperature and Wait
void GCodeGenerator::_print_first_layer_chamber_temperature(std::string &out, const Print &print, const std::string &gcode, uint16_t first_printing_extruder_id, bool wait)
{
    // Initial bed temperature based on the first extruder.
    int  temp = print.config().chamber_temperature.get_at(first_printing_extruder_id);
    //disable bed temp control if 0
    if (temp == 0) return;
    // Is the bed temperature set by the provided custom G-code?
    int  temp_by_gcode     = -1;
    bool temp_set_by_gcode = custom_gcode_sets_temperature(gcode, 141, 191, false, temp_by_gcode);
    if (temp_set_by_gcode && temp_by_gcode >= 0 && temp_by_gcode < 1000)
        temp = temp_by_gcode;
    // Always call m_writer.set_chamber_temperature() so it will set the internal "current" state of the chamber temp as if
    // the custom start G-code emited these.
    std::string set_temp_gcode = m_writer.set_chamber_temperature(temp, wait);
    if (!temp_set_by_gcode && !set_temp_gcode.empty())
        out += (set_temp_gcode);
}

// Write 1st layer extruder temperatures into the G-code.
// Only do that if the start G-code does not already contain any M-code controlling an extruder temperature.
// M104 - Set Extruder Temperature
// M109 - Set Extruder Temperature and Wait
// RepRapFirmware: G10 Sxx
void GCodeGenerator::_print_first_layer_extruder_temperatures(std::string &out, const Print &print, const std::string &gcode, uint16_t first_printing_extruder_id, bool wait)
{
    bool autoemit = print.config().autoemit_temperature_commands;
    // Is the bed temperature set by the provided custom G-code?
    int  temp_by_gcode     = -1;
    bool include_g10   = print.config().gcode_flavor.value == gcfRepRap;
    if (!autoemit || custom_gcode_sets_temperature(gcode, 104, 109, include_g10, temp_by_gcode)) {
        // Set the extruder temperature at m_writer, but throw away the generated G-code as it will be written with the custom G-code.
        int temp = print.config().first_layer_temperature.get_at(first_printing_extruder_id);
        if (temp == 0)
            temp = print.config().temperature.get_at(first_printing_extruder_id);
        if (autoemit && temp_by_gcode >= 0 && temp_by_gcode < 1000)
            temp = temp_by_gcode;
        //set writer, don't write gcode
        m_writer.set_temperature(temp, wait, first_printing_extruder_id);
    } else {
        // Custom G-code does not set the extruder temperature. Do it now.
        if (!print.config().single_extruder_multi_material.value) {
            // Set temperatures of all the printing extruders.
            for (const Extruder& tool : m_writer.extruders()) {
                int temp = print.config().first_layer_temperature.get_at(tool.id());
                if (temp == 0)
                    temp = print.config().temperature.get_at(tool.id());
                if (print.config().ooze_prevention.value && tool.id() != first_printing_extruder_id)
                    if (print.config().idle_temperature.is_nil(tool.id()))
                        temp += print.config().standby_temperature_delta.value;
                    else
                        temp = print.config().idle_temperature.get_at(tool.id());
                if (temp > 0)
                    out += (m_writer.set_temperature(temp, false, tool.id()));
            }
        }
        if (wait || print.config().single_extruder_multi_material.value) {
            // Set temperature of the first printing extruder only.
            int temp = print.config().first_layer_temperature.get_at(first_printing_extruder_id);
            if (temp == 0)
                temp = print.config().temperature.get_at(first_printing_extruder_id);
            if (temp > 0)
                out += (m_writer.set_temperature(temp, wait, first_printing_extruder_id));
        }
    }
}

std::vector<GCodeGenerator::InstanceToPrint> GCodeGenerator::sort_print_object_instances(
    const std::vector<ObjectLayerToPrint>       &object_layers,
	// Ordering must be defined for normal (non-sequential print).
	const std::vector<const PrintInstance*> 	*ordering,
	// For sequential print, the instance of the object to be printing has to be defined.
	const size_t                     		 	 single_object_instance_idx)
{
    std::vector<InstanceToPrint> out;

    if (ordering == nullptr) {
        // Sequential print, single object is being printed.
        assert(object_layers.size() == 1);
        out.emplace_back(0, *object_layers.front().object(), single_object_instance_idx);
    } else {
        // Create mapping from PrintObject* to ObjectLayerToPrint ID.
        std::vector<std::pair<const PrintObject*, size_t>> sorted;
        sorted.reserve(object_layers.size());
        for (const ObjectLayerToPrint &object : object_layers)
            if (const PrintObject* print_object = object.object(); print_object)
                sorted.emplace_back(print_object, &object - object_layers.data());
		std::sort(sorted.begin(), sorted.end());

		if (! sorted.empty()) {
		    out.reserve(sorted.size());
		    for (const PrintInstance *instance : *ordering) {
		    	const PrintObject &print_object = *instance->print_object;
		    	std::pair<const PrintObject*, size_t> key(&print_object, 0);
		    	auto it = std::lower_bound(sorted.begin(), sorted.end(), key);
		    	if (it != sorted.end() && it->first == &print_object)
		    		// ObjectLayerToPrint for this PrintObject was found.
					out.emplace_back(it->second, print_object, instance - print_object.instances().data());
		    }
		}
	}
	return out;
}

namespace ProcessLayer
{

    static std::string emit_custom_color_change_gcode_per_print_z(
        GCodeGenerator          &gcodegen,
        const CustomGCode::Item &custom_gcode,
        unsigned int             current_extruder_id,
        unsigned int             first_extruder_id, // ID of the first extruder printing this layer.
        const Print              &print,
        Print::StatusMonitor     &status_emitter
    ) {
        const bool single_extruder_multi_material = print.config().single_extruder_multi_material;
        const bool single_extruder_printer        = print.config().nozzle_diameter.size() == 1;
        const bool color_change                   = custom_gcode.type == CustomGCode::ColorChange;

        std::string gcode;

        int color_change_extruder = -1;
        if (color_change && custom_gcode.extruder > 0)
            color_change_extruder = custom_gcode.extruder - 1;

        if (color_change) {
            //update stats : length
            double previously_extruded = 0;
            for (const auto &tuple : status_emitter.stats().color_extruderid_to_used_filament)
                if (tuple.first == color_change_extruder)
                    previously_extruded += tuple.second;
            status_emitter.stats().color_extruderid_to_used_filament.emplace_back(
                color_change_extruder,
                gcodegen.writer().get_tool(color_change_extruder)->used_filament() - previously_extruded);
        }

        assert(color_change_extruder >= 0);
        // Color Change or Tool Change as Color Change.
        // add tag for processor
        gcode += ";" + GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Color_Change) + ",T" + std::to_string(color_change_extruder) + "," + custom_gcode.color + "\n";

        DynamicConfig cfg;
        cfg.set_key_value("color_change_extruder", new ConfigOptionInt(color_change_extruder));
        cfg.set_key_value("next_colour", new ConfigOptionString(custom_gcode.color));
        if (single_extruder_multi_material && !single_extruder_printer && color_change_extruder >= 0 && first_extruder_id != unsigned(color_change_extruder)) {
            //! FIXME_in_fw show message during print pause
            // FIXME: Why is pause_print_gcode here? Why is it supplied "color_change_extruder"?
            gcode += gcodegen.placeholder_parser_process("pause_print_gcode",
                                                         GCodeWriter::get_default_pause_gcode(print.config()),
                                                         current_extruder_id, &cfg);
            gcode += "\n";
            if (auto flavor = print.config().gcode_flavor.value; 
                flavor == gcfKlipper || flavor == gcfMarlinFirmware || flavor == gcfMarlinLegacy || flavor == gcfRepetier || flavor == gcfSmoothie || flavor == gcfRepRap)
                gcode += "M117 Change filament for Extruder " + std::to_string(color_change_extruder) + "\n";
        } else {
            if (GCodeWriter::get_default_color_change_gcode(print.config()).empty()) {
                status_emitter.active_step_add_warning(
                    PrintStateBase::WarningLevel::NON_CRITICAL,
                                                _u8L("Using a color change gcode, but there isn't one for this printer."
                                                    "\nThe printer won't stop for the filament change, unless you set it manually in the custom gcode section."));
            }
            cfg.set_key_value("color_change_extruder", new ConfigOptionInt(int32_t(current_extruder_id))); // placeholder to avoid 'crashs'
            gcode += gcodegen.placeholder_parser_process("color_change_gcode",
                                                         GCodeWriter::get_default_color_change_gcode(print.config()),
                                                         current_extruder_id, &cfg);
            gcode += "\n";
            //FIXME Tell G-code writer that M600 filled the extruder, thus the G-code writer shall reset the extruder to unretracted state after
            // return from M600. Thus the G-code generated by the following line is ignored.
            // see GH issue #6362
            // merill: don't unretract, as it create problem on absolute position. Clear the retraction properly, plz.
            gcodegen.writer().tool()->reset_retract();
        }

        return gcode;
    }

    std::string emit_custom_gcode_per_print_z(
        GCodeGenerator                                          &gcodegen,
        const CustomGCode::Item                                 &custom_gcode,
        uint16_t                                                 current_extruder_id,
        // ID of the first extruder printing this layer.
        uint16_t                                                 first_extruder_id,
        const Print                                             &print,
        Print::StatusMonitor                                    &status_emitter)
    {
        std::string gcode;

        // Extruder switches are processed by LayerTools, they should be filtered out.
        assert(custom_gcode.type != CustomGCode::ToolChange);

        CustomGCode::Type gcode_type   = custom_gcode.type;
        const bool        color_change = gcode_type == CustomGCode::ColorChange;
        const bool        tool_change  = gcode_type == CustomGCode::ToolChange;
        // Tool Change is applied as Color Change for a single extruder printer only.
        assert(!tool_change || print.config().nozzle_diameter.size() == 1);

        // we should add or not colorprint_change in respect to nozzle_diameter count instead of really used extruders count
        if (color_change || tool_change) {
            gcode += emit_custom_color_change_gcode_per_print_z(gcodegen, custom_gcode, current_extruder_id, first_extruder_id, print, status_emitter);
        } else {
            if (gcode_type == CustomGCode::PausePrint) { // Pause print
                const std::string pause_print_msg = custom_gcode.extra;

                if (GCodeWriter::get_default_pause_gcode(print.config()).empty()) {
                    status_emitter.active_step_add_warning(
                        PrintStateBase::WarningLevel::NON_CRITICAL,
                        _u8L("Using a pause gcode, but there isn't one for this printer."
                            "\nThe printer won't pause, unless you set it manually in the custom gcode section."));
                }

                // add tag for processor
                gcode += ";" + GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Pause_Print) + "\n";
                //! FIXME_in_fw show message during print pause
                if (!pause_print_msg.empty())
                    if (auto flavor = print.config().gcode_flavor.value; 
                        flavor == gcfKlipper ||
                        flavor == gcfMarlinFirmware || flavor == gcfMarlinLegacy || flavor == gcfRepetier ||
                        flavor == gcfSmoothie || flavor == gcfRepRap)
                        gcode += "M117 " + pause_print_msg + "\n";

                DynamicConfig cfg;
                cfg.set_key_value("color_change_extruder", new ConfigOptionInt(int(current_extruder_id)));
                gcode += gcodegen.placeholder_parser_process("pause_print_gcode",
                                                             GCodeWriter::get_default_pause_gcode(print.config()),
                                                             current_extruder_id, &cfg);
            } else {
                // add tag for processor
                gcode += ";" + GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Custom_Code) + "\n";
                if (gcode_type == CustomGCode::Template)    // Template Custom Gcode
                    gcode += gcodegen.placeholder_parser_process("template_custom_gcode",
                                                                 print.config().template_custom_gcode,
                                                                 current_extruder_id);
                else                                        // custom Gcode
                    gcode += custom_gcode.extra;
            }
            gcode += "\n";
        }

        return gcode;
    }

} // namespace ProcessLayer

namespace Skirt {
	static void skirt_loops_per_extruder_all_printing(const Print &print, const LayerTools &layer_tools, std::map<uint16_t, std::pair<size_t, size_t>> &skirt_loops_per_extruder_out)
	{
        // Prime all extruders printing over the 1st layer over the skirt lines.
        size_t n_loops = print.skirt().entities().size();
        size_t n_tools = layer_tools.extruders.size();
        size_t lines_per_extruder = (n_loops + n_tools - 1) / n_tools;
        for (size_t i = 0; i < n_loops; i += lines_per_extruder)
            skirt_loops_per_extruder_out[layer_tools.extruders[i / lines_per_extruder]] = std::pair<size_t, size_t>(i, std::min(i + lines_per_extruder, n_loops));
	}

    static std::map<uint16_t, std::pair<size_t, size_t>> make_skirt_loops_per_extruder_1st_layer(
        const Print             				&print,
        const LayerTools                		&layer_tools,
        // Heights (print_z) at which the skirt has already been extruded.
        std::vector<coordf_t>  			    	&skirt_done)
    {
        // Extrude skirt at the print_z of the raft layers and normal object layers
        // not at the print_z of the interlaced support material layers.
        std::map<uint16_t, std::pair<size_t, size_t>> skirt_loops_per_extruder_out;
        //For sequential print, the following test may fail when extruding the 2nd and other objects.
        // assert(skirt_done.empty());
        if (skirt_done.empty() && print.has_skirt() && ! print.skirt().entities().empty() && layer_tools.has_skirt) {
            if (print.skirt_first_layer()) {
                size_t n_loops = print.skirt_first_layer()->entities().size();
                size_t n_tools = layer_tools.extruders.size();
                size_t lines_per_extruder = (n_loops + n_tools - 1) / n_tools;
                for (size_t i = 0; i < n_loops; i += lines_per_extruder)
                    skirt_loops_per_extruder_out[layer_tools.extruders[i / lines_per_extruder]] = std::pair<size_t, size_t>(i, std::min(i + lines_per_extruder, n_loops));
            } else
                skirt_loops_per_extruder_all_printing(print, layer_tools, skirt_loops_per_extruder_out);
            skirt_done.emplace_back(layer_tools.print_z);
        }
        return skirt_loops_per_extruder_out;
    }

    static std::map<uint16_t, std::pair<size_t, size_t>> make_skirt_loops_per_extruder_other_layers(
        const Print 							&print,
        const LayerTools                		&layer_tools,
        // Heights (print_z) at which the skirt has already been extruded.
        std::vector<coordf_t>			    	&skirt_done)
    {
        // Extrude skirt at the print_z of the raft layers and normal object layers
        // not at the print_z of the interlaced support material layers.
        std::map<uint16_t, std::pair<size_t, size_t>> skirt_loops_per_extruder_out;
        if (print.has_skirt() && ! print.skirt().entities().empty() && layer_tools.has_skirt &&
            // infinite or high skirt does not make sense for sequential print here
            //(if it is selected, it's done in the "extrude object-only skirt" in process_layer)
            // Not enough skirt layers printed yet.
            (skirt_done.size() < (size_t)print.config().skirt_height.value || print.has_infinite_skirt())) {
            bool valid = ! skirt_done.empty() && skirt_done.back() < layer_tools.print_z - EPSILON;
            assert(valid);
            // This print_z has not been extruded yet (sequential print)
            // FIXME: The skirt_done should not be empty at this point. The check is a workaround
            // of https://github.com/prusa3d/PrusaSlicer/issues/5652, but it deserves a real fix.
            if (valid) {
#if 0
                // Prime just the first printing extruder. This is original Slic3r's implementation.
                skirt_loops_per_extruder_out[layer_tools.extruders.front()] = std::pair<size_t, size_t>(0, print.config().skirts.value);
#else
                // Prime all extruders planned for this layer, see
                // https://github.com/prusa3d/PrusaSlicer/issues/469#issuecomment-322450619
                skirt_loops_per_extruder_all_printing(print, layer_tools, skirt_loops_per_extruder_out);
#endif
                assert(!skirt_done.empty());
                skirt_done.emplace_back(layer_tools.print_z);
            }
        }
        return skirt_loops_per_extruder_out;
    }

} // namespace Skirt

bool GCodeGenerator::line_distancer_is_required(const std::vector<uint16_t>& extruder_ids) {
    for (const uint16_t id : extruder_ids) {
        const double travel_slope{this->m_config.travel_slope.get_at(id)};
        if (
            this->m_config.travel_lift_before_obstacle.get_at(id)
            && this->m_config.travel_max_lift.get_at(id) > 0
            && travel_slope > 0
            && travel_slope < 90
        ) {
            return true;
        }
    }
    return false;
}

std::string GCodeGenerator::get_layer_change_gcode(const Vec3d& from, const Vec3d& to, const unsigned extruder_id) {
    const Polyline xy_path{
        this->gcode_to_point(from.head<2>()),
        this->gcode_to_point(to.head<2>())
    };

    using namespace GCode::Impl::Travels;

    ElevatedTravelParams elevation_params{
        get_elevated_traval_params(xy_path, this->m_config, extruder_id, this->m_travel_obstacle_tracker)};

    const double initial_elevation = from.z();
    const double z_change = to.z() - from.z();
    elevation_params.lift_height = std::max(z_change, elevation_params.lift_height);

    const double path_length = unscaled(xy_path.length());
    const double lift_at_travel_end = std::min(
        elevation_params.lift_height,
        elevation_params.lift_height / elevation_params.slope_end * path_length
    );
    if (lift_at_travel_end < z_change) {
        elevation_params.lift_height = z_change;
        elevation_params.slope_end = path_length;
    }

    const std::vector<double> ensure_points_at_distances = linspace(
        elevation_params.slope_end - elevation_params.blend_width / 2.0,
        elevation_params.slope_end + elevation_params.blend_width / 2.0,
        elevation_params.parabola_points_count
    );

    Points3 travel{generate_elevated_travel(
        xy_path.points, ensure_points_at_distances, initial_elevation,
        ElevatedTravelFormula{elevation_params}
    )};

    std::string travel_gcode;
    Vec3d previous_point{this->point_to_gcode(travel.front())};
    for (const Vec3crd& point : travel) {
        const Vec3d gcode_point{this->point_to_gcode(point)};
        assert(this->m_writer.get_position() == previous_point);
        travel_gcode += this->m_writer.travel_to_xyz(gcode_point, 0.0, "layer change");
        previous_point = gcode_point;
    }
    return travel_gcode;
}

// Matches "G92 E0" with various forms of writing the zero and with an optional comment.
std::regex regex_g92e0_gcode{ "^[ \\t]*[gG]92[ \\t]*[eE](0(\\.0*)?|\\.0+)[ \\t]*(;.*)?$" };

// In sequential mode, process_layer is called once per each object and its copy,
// therefore layers will contain a single entry and single_object_instance_idx will point to the copy of the object.
// In non-sequential mode, process_layer is called per each print_z height with all object and support layers accumulated.
// For multi-material prints, this routine minimizes extruder switches by gathering extruder specific extrusion paths
// and performing the extruder specific extrusions together.
LayerResult GCodeGenerator::process_layer(
    const Print                             &print,
    Print::StatusMonitor                    &status_monitor,
    // Set of object & print layers of the same PrintObject and with the same print_z.
    const ObjectsLayerToPrint               &layers,
    const LayerTools                        &layer_tools,
    const bool                               last_layer,
    // Pairs of PrintObject index and its instance index.
    const std::vector<const PrintInstance*> *ordering,
    // If set to size_t(-1), then print all copies of all objects.
    // Otherwise print a single copy of a single object.
    const size_t                     		 single_object_instance_idx)
{
    assert(!layers.empty());
    // Either printing all copies of all objects, or just a single copy of a single object.
    assert(single_object_instance_idx == size_t(-1) || layers.size() == 1);
    if(single_object_instance_idx != size_t(-1))
        m_print_object_instance_id = static_cast<uint16_t>(single_object_instance_idx);

    // First object, support and raft layer, if available.
    const Layer         *object_layer  = nullptr;
    const SupportLayer  *support_layer = nullptr;
    const SupportLayer  *raft_layer    = nullptr;
    /*const*/ size_t layer_id = size_t(-1);
    for (const ObjectLayerToPrint &l : layers) {
        if(l.layer())
            layer_id = l.layer()->id();
        if (l.object_layer && ! object_layer)
            object_layer = l.object_layer;
        if (l.support_layer) {
            if (! support_layer)
                support_layer = l.support_layer;
            if (! raft_layer && support_layer->id() < support_layer->object()->slicing_parameters().raft_layers())
                raft_layer = support_layer;
        }
        assert(l.layer() == nullptr || layer_id == l.layer()->id());
    }
    assert(layer_id < layer_count());
    assert(object_layer != nullptr || support_layer != nullptr);
    const Layer         &layer         = (object_layer != nullptr) ? *object_layer : *support_layer;
    assert(layer_id == layer.id());
    LayerResult   result { {}, layer.id(), false, last_layer, false};
    if (layer_tools.extruders.empty())
        // Nothing to extrude.
        return result;

    if (object_layer) {
        if (single_object_instance_idx != size_t(-1)) {
            size_t nb_layers = object_layer->object()->layer_count();
            m_object_sequentially_printed.insert(object_layer->object());
            print.set_status(int((layer.id() * 100) / nb_layers),
                             std::string(L("Generating G-code layer %s / %s for object %s / %s")),
                             std::vector<std::string>{std::to_string(layer.id()), std::to_string(nb_layers), std::to_string(m_object_sequentially_printed.size()), std::to_string(print.num_object_instances())},
                             PrintBase::SlicingStatus::DEFAULT | PrintBase::SlicingStatus::SECONDARY_STATE);
        } else {
            print.set_status(int((layer.id() * 100) / layer_count()),
                             std::string(L("Generating G-code layer %s / %s")),
                             std::vector<std::string>{std::to_string(layer.id()), std::to_string(layer_count())},
                             PrintBase::SlicingStatus::DEFAULT | PrintBase::SlicingStatus::SECONDARY_STATE);
        }
    }

    // Extract 1st object_layer and support_layer of this set of layers with an equal print_z.
    coordf_t             print_z       = layer.print_z + m_config.z_offset.value;
    bool                 first_layer   = layer_id == 0;
    uint16_t             first_extruder_id = layer_tools.extruders.front();

    // Initialize config with the 1st object to be printed at this layer.
    m_config.apply(print.default_region_config(), true);
    m_config.apply(layer.object()->config(), true);

    // Check whether it is possible to apply the spiral vase logic for this layer.
    // Just a reminder: A spiral vase mode is allowed for a single object, single material print only.
    m_enable_loop_clipping = true;
    if (m_spiral_vase && layers.size() == 1 && support_layer == nullptr) {
        bool enable = (layer.id() > 0 || !layer.object()->has_brim()) && (layer.id() >= (size_t)print.config().skirt_height.value && ! print.has_infinite_skirt());
        if (enable) {
            for (const LayerRegion *layer_region : layer.regions())
                if (size_t(layer_region->region().config().bottom_solid_layers.value) > layer.id() ||
                    layer_region->perimeters().items_count() > 1u ||
                    layer_region->fills().items_count() > 0 ||
                    // there should be a fills if there is an ironings anyway
                    layer_region->ironings().items_count() > 0) {
                    enable = false;
                    break;
                }
        }
        result.spiral_vase_enable = enable;
        if (enable)
            m_spiral_vase_layer = std::abs(m_spiral_vase_layer) + 1;
        else
            m_spiral_vase_layer = -std::abs(m_spiral_vase_layer);
        // If we're going to apply spiralvase to this layer, disable loop clipping.
        m_enable_loop_clipping = !enable;
    }


    std::string gcode;
    assert(is_decimal_separator_point()); // for the sprintfs

    // add tag for processor
    gcode += ";" + GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Layer_Change) + "\n";
    // export layer z
    gcode += std::string(";Z:") + float_to_string_decimal_point(print_z) + "\n";

    // export layer height
    double height = first_layer ? print_z : print_z - m_last_layer_z;
    gcode += std::string(";") + GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Height)
        + float_to_string_decimal_point(height) + "\n";

    // update caches
    const double previous_layer_z{m_last_layer_z};
    m_last_layer_z = print_z;
    m_max_layer_z  = std::max(m_max_layer_z, m_last_layer_z);
    m_last_height = height;
    m_last_too_small.polyline.clear();
    m_current_layer_first_position = std::nullopt;
    //m_already_unretracted = false;

    // Set new layer - this will change Z and force a retraction if retract_layer_change is enabled.
    assert(std::abs(previous_layer_z - (m_layer != nullptr ? m_layer->print_z : 0)) < 0.00000001);
    if (! print.config().before_layer_gcode.value.empty()) {
        DynamicConfig config;
        config.set_key_value("previous_layer_z", new ConfigOptionFloat(previous_layer_z));
        config.set_key_value("layer_num", new ConfigOptionInt(m_layer_index + 1));
        config.set_key_value("layer_z",     new ConfigOptionFloat(print_z));
        config.set_key_value("max_layer_z", new ConfigOptionFloat(m_max_layer_z));
        gcode += this->placeholder_parser_process("before_layer_gcode",
            print.config().before_layer_gcode.value, m_writer.tool()->id(), &config)
            + "\n";
    }
    // print z move to next layer UNLESS (HACK for superslicer#1775)
    // if it's going to the first layer, then we may want to delay the move in these condition:
    // there is no "after layer change gcode" and it's the first move from the unknown
    if (print.config().layer_gcode.value.empty() && !last_pos_defined() && m_config.start_gcode_manual && (
            // there is a lift (on the first llyer, so the first move will bring us to the required height
        (m_writer.tool()->retract_lift() > 0 && (m_config.retract_lift_above.get_at(m_writer.tool()->id()) == 0 || BOOL_EXTRUDER_CONFIG(retract_lift_first_layer)))
        ||   // or lift_min is higher than the first layer height.
         m_config.lift_min.value > layer.print_z
            )) {
        // still do the retraction
        gcode += m_writer.retract();
        gcode += m_writer.reset_e();
        m_delayed_layer_change = this->change_layer(previous_layer_z, print_z, result.spiral_vase_enable); //HACK for superslicer#1775
    } else {
        //extra lift on layer change if multiple objects
        if(single_object_instance_idx == size_t(-1) && (support_layer != nullptr || layers.size() > 1))
            set_extra_lift(m_last_layer_z, layer.id(), print.config(), m_writer, first_extruder_id);
        gcode += this->change_layer(previous_layer_z, print_z, result.spiral_vase_enable);  // this will increase m_layer_index
    }
    m_layer = &layer;
    if (this->line_distancer_is_required(layer_tools.extruders) && this->m_layer != nullptr && this->m_layer->lower_layer != nullptr)
        m_travel_obstacle_tracker.init_layer(layer, layers);

    m_object_layer_over_raft = false;
    if (!first_layer && ! print.config().layer_gcode.value.empty()) {
        DynamicConfig config;
        config.set_key_value("previous_layer_z", new ConfigOptionFloat(previous_layer_z));
        config.set_key_value("layer_num", new ConfigOptionInt(m_layer_index));
        config.set_key_value("layer_z",   new ConfigOptionFloat(print_z));
        config.set_key_value("max_layer_z", new ConfigOptionFloat(m_max_layer_z));
        gcode += this->placeholder_parser_process("layer_gcode",
            print.config().layer_gcode.value, m_writer.tool()->id(), &config)
            + "\n";
    }

    //put G92 E0 is relative extrusion
    bool before_layer_gcode_resets_extruder = std::regex_search(print.config().before_layer_gcode.value, regex_g92e0_gcode);
    bool layer_gcode_resets_extruder = std::regex_search(print.config().layer_gcode.value, regex_g92e0_gcode);
    if (m_config.use_relative_e_distances) {
        // See GH issues prusa#6336 #5073$
        if (!before_layer_gcode_resets_extruder && !layer_gcode_resets_extruder) {
            gcode += "G92 E0";
            if (print.config().gcode_comments) {
                gcode += " ; reset extruder position to flush any extruder axis rounding";
            }
            gcode += "\n";
        }
    }

    if (! first_layer && ! m_second_layer_things_done) {
        // Transition from 1st to 2nd layer. Adjust nozzle temperatures as prescribed by the nozzle dependent
        // first_layer_temperature vs. temperature settings.
        for (const Extruder &extruder : m_writer.extruders()) {
            if (print.config().single_extruder_multi_material.value || m_ooze_prevention.enable) {
                // In single extruder multi material mode, set the temperature for the current extruder only.
                // The same applies when ooze prevention is enabled.
                if (extruder.id() != m_writer.tool()->id())
                    continue;
            }
            int temperature = print.config().temperature.get_at(extruder.id());
            if (temperature > 0) // don't set it if disabled
                gcode += m_writer.set_temperature(temperature, false, extruder.id());
        }
        if (print.config().bed_temperature.get_at(first_extruder_id) > 0)  // don't set it if disabled
            gcode += m_writer.set_bed_temperature(print.config().bed_temperature.get_at(first_extruder_id));
        // Mark the temperature transition from 1st to 2nd layer to be finished.
        m_second_layer_things_done = true;
    }

    // Map from extruder ID to <begin, end> index of skirt loops to be extruded with that extruder.
    std::map<uint16_t, std::pair<size_t, size_t>> skirt_loops_per_extruder;

    // Extrude skirt at the print_z of the raft layers and normal object layers
    // not at the print_z of the interlaced support material layers.
    skirt_loops_per_extruder = first_layer ?
        Skirt::make_skirt_loops_per_extruder_1st_layer(print, layer_tools, m_skirt_done) :
        Skirt::make_skirt_loops_per_extruder_other_layers(print, layer_tools, m_skirt_done);

    if (this->config().avoid_crossing_curled_overhangs) {
        m_avoid_crossing_curled_overhangs.clear();
        for (const ObjectLayerToPrint &layer_to_print : layers) {
            if (layer_to_print.object() == nullptr)
                continue;
            for (const auto &instance : layer_to_print.object()->instances()) {
                m_avoid_crossing_curled_overhangs.add_obstacles(layer_to_print.object_layer, instance.shift);
                m_avoid_crossing_curled_overhangs.add_obstacles(layer_to_print.support_layer, instance.shift);
            }
        }
    }

    const bool has_custom_gcode_to_emit     = single_object_instance_idx == size_t(-1) && layer_tools.custom_gcode != nullptr;
    const int  extruder_id_for_custom_gcode = int(layer_tools.extruder_needed_for_color_changer) - 1;

    if (has_custom_gcode_to_emit && extruder_id_for_custom_gcode == -1) {
        // Normal (non-sequential) print with some custom code without picking a specific extruder before it.
        // If we don't need to pick a specific extruder before the color change, we can just emit a custom g-code.
        // Otherwise, we will emit the g-code after picking the specific extruder.

        std::string custom_gcode = ProcessLayer::emit_custom_gcode_per_print_z(*this, *layer_tools.custom_gcode, m_writer.tool()->id(), first_extruder_id, print, status_monitor);
        if (layer_tools.custom_gcode->type == CustomGCode::ColorChange) {
            // We have a color change to do on this layer, but we want to do it immediately before the first extrusion instead of now, in order to fix GH #2672.
            m_pending_pre_extrusion_gcode = custom_gcode;
        } else {
            gcode += custom_gcode;
        }
    }

    // Extrude the skirt, brim, support, perimeters, infill ordered by the extruders.
    for (uint16_t extruder_id : layer_tools.extruders)
    {
        gcode += (layer_tools.has_wipe_tower && m_wipe_tower) ?
            m_wipe_tower->tool_change(*this, extruder_id, extruder_id == layer_tools.extruders.back()) :
            this->set_extruder(extruder_id, print_z);

        // let analyzer tag generator aware of a role type change
        if (layer_tools.has_wipe_tower && m_wipe_tower)
            m_last_processor_extrusion_role = GCodeExtrusionRole::WipeTower;

        if (has_custom_gcode_to_emit && extruder_id_for_custom_gcode == int(extruder_id)) {
            assert(m_writer.tool()->id() == extruder_id_for_custom_gcode);
            assert(m_pending_pre_extrusion_gcode.empty());
            // Now we have picked the right extruder, so we can emit the custom g-code.
            gcode += ProcessLayer::emit_custom_gcode_per_print_z(*this, *layer_tools.custom_gcode, m_writer.tool()->id(), first_extruder_id, print, status_monitor);
        }

        //if first layer, ask for a bigger lift for travel to object, to be on the safe side
        if(single_object_instance_idx == size_t(-1) && m_layer_index == 0)
            set_extra_lift(m_last_layer_z, layer.id(), print.config(), m_writer, extruder_id);

        if (auto loops_it = skirt_loops_per_extruder.find(extruder_id); loops_it != skirt_loops_per_extruder.end()) {
            //global skirt & brim use the global settings.
            //m_config.apply(print.default_object_config(), true);
            apply_print_configs(print);
            // before going to and from a global skirt, please ensure you are a a safe height
            set_extra_lift(m_last_layer_z, layer.id(), print.config(), m_writer, extruder_id);
            const std::pair<size_t, size_t> loops = loops_it->second;
            this->set_origin(0., 0.);
            m_avoid_crossing_perimeters.use_external_mp();
            Flow layer_skirt_flow = print.skirt_flow(extruder_id).with_height(float(m_skirt_done.back() - (m_skirt_done.size() == 1 ? 0. : m_skirt_done[m_skirt_done.size() - 2])));
            double mm3_per_mm = layer_skirt_flow.mm3_per_mm();
            const ExtrusionEntityCollection& coll = first_layer && print.skirt_first_layer() ? *print.skirt_first_layer() : print.skirt();
            for (size_t i = loops.first; i < loops.second; ++i) {
                m_region = nullptr;
                set_region_for_extrude(print, nullptr, gcode);
                // Adjust flow according to this layer's layer height.
                this->extrude_skirt(dynamic_cast<ExtrusionLoop&>(*print.skirt().entities()[i]),
                    // Override of skirt extrusion parameters. extrude_skirt() will fill in the extrusion width.
                    ExtrusionFlow{ mm3_per_mm, 0., layer_skirt_flow.height() }, gcode, "skirt"sv);
            }
            m_last_too_small.polyline.clear();
            m_avoid_crossing_perimeters.use_external_mp(false);
            // Allow a straight travel move to the first object point if this is the first layer (but don't in next layers).
            if (first_layer && loops.first == 0)
                m_avoid_crossing_perimeters.disable_once();
            // before going to and from a global skirt, please ensure you are a a safe height
            set_extra_lift(m_last_layer_z, layer.id(), print.config(), m_writer, extruder_id);
        }

        // Extrude brim with the extruder of the 1st region.
        if (! m_brim_done) {
            //global skirt & brim use the global settings.
            m_config.apply(print.default_object_config(), true);
            this->set_origin(0., 0.);
            m_avoid_crossing_perimeters.use_external_mp();
            m_region = nullptr;
            set_region_for_extrude(print, nullptr, gcode);
            for (const ExtrusionEntity* brim_entity : print.brim().entities()) {
                //if first layer, ask for a bigger lift for travel to each brim, to be on the safe side
                set_extra_lift(m_last_layer_z, layer.id(), print.config(), m_writer, extruder_id);
                gcode += this->extrude_entity({*brim_entity, false}, "Brim"sv);
            }
            m_last_too_small.polyline.clear();
            m_brim_done = true;
            m_avoid_crossing_perimeters.use_external_mp(false);
            // Allow a straight travel move to the first object point.
            m_avoid_crossing_perimeters.disable_once();
            //to go to the object-only skirt or brim, or to the object  (May be overriden here but I don't care)
            set_extra_lift(m_last_layer_z, layer.id(), print.config(), m_writer, extruder_id);
        }
        bool print_object_skirtbrim_start = print.config().complete_objects.value || print.config().parallel_objects_step > 0;
        //extrude object-only skirt (for sequential)
        //TODO: use it also for wiping like the other one (as they are exlusiev)
        if (print_object_skirtbrim_start && !layers.front().object()->skirt().empty()
            && extruder_id == layer_tools.extruders.front() && object_layer) {

            const PrintObject *print_object = layers.front().object();
            //object skirt & brim use the object settings.
            m_region = nullptr;
            set_region_for_extrude(print, print_object, gcode);
            this->set_origin(unscale(print_object->instances()[single_object_instance_idx].shift));
            if (this->m_layer != nullptr && (this->m_layer->id() < m_config.skirt_height || print.has_infinite_skirt() )) {
                //TODO: check if I don't need to call extrude_skirt to have arcs.
                if(first_layer && print.skirt_first_layer())
                    for (const ExtrusionEntity* ee : print_object->skirt_first_layer()->entities())
                        gcode += this->extrude_entity({*ee, false}, "");
                else
                    for (const ExtrusionEntity *ee : print_object->skirt().entities())
                        gcode += this->extrude_entity({*ee, false}, "");
                m_last_too_small.polyline.clear();
            }
        }
        //extrude object-only brim (for sequential)
        if (print_object_skirtbrim_start && !layers.front().object()->brim().empty()
            && extruder_id == layer_tools.extruders.front() && object_layer) {

            const PrintObject* print_object = layers.front().object();
            //object skirt & brim use the object settings.
            m_region = nullptr;
            set_region_for_extrude(print, print_object, gcode);
            this->set_origin(unscale(print_object->instances()[single_object_instance_idx].shift));
            if (this->m_layer != nullptr && this->m_layer->id() == 0) {
                m_avoid_crossing_perimeters.use_external_mp(true);
                for (const ExtrusionEntity* ee : print_object->brim().entities())
                    gcode += this->extrude_entity({*ee, false}, "Brim"sv);
                m_avoid_crossing_perimeters.use_external_mp(false);
                m_avoid_crossing_perimeters.disable_once();
                m_last_too_small.polyline.clear();
            }
            
        }

        std::vector<InstanceToPrint> instances_to_print = sort_print_object_instances(layers, ordering, single_object_instance_idx);

        // We are almost ready to print. However, we must go through all the objects twice to print the the overridden extrusions first (infill/perimeter wiping feature):
        bool is_anything_overridden = layer_tools.wiping_extrusions().is_anything_overridden();
        if (is_anything_overridden) {
            // Extrude wipes.
            size_t gcode_size_old = gcode.size();
            for (const InstanceToPrint &instance : instances_to_print) {
                this->process_layer_single_object(
                    gcode, 
                    ExtrudeArgs{extruder_id, instance, layer_tools, is_anything_overridden, true /* print_wipe_extrusions */},

                    layers[instance.object_layer_to_print_id]);
            }
            if (gcode_size_old < gcode.size())
                gcode+="; PURGING FINISHED\n";
        }
        // Extrude normal extrusions.
        for (const InstanceToPrint &instance : instances_to_print) {
            this->process_layer_single_object(
                gcode, ExtrudeArgs{extruder_id, instance, layer_tools, is_anything_overridden, false /* print_wipe_extrusions */},
                layers[instance.object_layer_to_print_id]);
        }
    }
    
    emit_milling_commands(gcode, layers);


    // During layer change the starting position of next layer is now known.
    // The solution is thus to emplace a temporary tag to the gcode, cache the postion and
    // replace the tag later. The tag is Layer_Change_Travel, the cached position is
    // m_current_layer_first_position and it is replaced here.
    const std::string tag = GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Layer_Change_Travel);
    std::string layer_change_gcode;
    const bool do_ramping_layer_change = (
        m_previous_layer_last_position
        && m_current_layer_first_position
        && m_layer_change_extruder_id
        && !result.spiral_vase_enable
        && print_z > previous_layer_z
        && this->m_config.travel_ramping_lift.get_at(*m_layer_change_extruder_id)
        && this->m_config.travel_slope.get_at(*m_layer_change_extruder_id) > 0
        && this->m_config.travel_slope.get_at(*m_layer_change_extruder_id) < 90
    );
    if (first_layer) {
        layer_change_gcode = ""; // Explicit for readability.
    } else if (do_ramping_layer_change) {
        layer_change_gcode = this->get_layer_change_gcode(*m_previous_layer_last_position, *m_current_layer_first_position, *m_layer_change_extruder_id);
    } else {
        layer_change_gcode = this->writer().get_travel_to_z_gcode(print_z, "simple layer change");
    }

    const auto keep_retraciton{[&](){
        if (!do_ramping_layer_change) {
            return true;
        }
        const double travel_length{(*m_current_layer_first_position - *m_previous_layer_last_position_before_wipe).norm()};
        if (this->m_config.retract_before_travel.get_at(*m_layer_change_extruder_id) < travel_length) {
            // Travel is long, keep retraction.
            return true;
        }
        return false;
    }};

    bool removed_retraction{false};
    if (this->m_config.travel_ramping_lift.get_at(*m_layer_change_extruder_id) && !result.spiral_vase_enable) {
        const std::string retraction_start_tag = GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Layer_Change_Retraction_Start);
        const std::string retraction_end_tag = GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Layer_Change_Retraction_End);

        if (keep_retraciton()) {
            boost::algorithm::replace_first(gcode, retraction_start_tag, "");
            boost::algorithm::replace_first(gcode, retraction_end_tag, "");
        } else {
            const std::size_t start{gcode.find(retraction_start_tag)};
            const std::size_t end_tag_start{gcode.find(retraction_end_tag)};
            const std::size_t end{end_tag_start + retraction_end_tag.size()};
            gcode.replace(start, end - start, "");

            layer_change_gcode = this->get_layer_change_gcode(*m_previous_layer_last_position_before_wipe, *m_current_layer_first_position, *m_layer_change_extruder_id);

            removed_retraction = true;
        }
    }

    if (removed_retraction) {
        const std::size_t start{gcode.find("FIRST_UNRETRACT")};
        const std::size_t end{gcode.find("\n", start)};
        gcode.replace(start, end - start, "");
    } else {
        boost::algorithm::replace_first(gcode,"FIRST_UNRETRACT", "");
    }

    boost::algorithm::replace_first(gcode, tag, layer_change_gcode);

    // set area used in this layer
    double layer_area = 0;
    for (const GCode::ObjectLayerToPrint &print_layer : layers) {
        //note: a layer can be null if the objetc doesn't have aanything to print at this height.
        if (print_layer.layer())
            for (auto poly : print_layer.layer()->lslices) layer_area += poly.area();
    }
    layer_area = unscaled(unscaled(layer_area));
    status_monitor.stats().layer_area_stats.emplace_back(print_z, layer_area);

    BOOST_LOG_TRIVIAL(trace) << "Exported layer " << layer.id() << " print_z " << print_z <<
    log_memory_info();

    result.gcode = std::move(gcode);
    result.cooling_buffer_flush = object_layer || raft_layer || last_layer;
    return result;
}

static const auto comment_perimeter = "perimeter"sv;
// Comparing string_view pointer & length for speed.
static inline bool comment_is_perimeter(const std::string_view comment) {
    return comment.data() == comment_perimeter.data() && comment.size() == comment_perimeter.size();
}

void GCodeGenerator::process_layer_single_object(
    // output
    std::string              &gcode, 
    // What object and instance is going to be printed.
    // Index of the extruder currently active.
    // Container for extruder overrides (when wiping into object or infill).
    // Is any extrusion possibly marked as wiping extrusion?
    // Round 1 (wiping into object or infill) or round 2 (normal extrusions).
    const ExtrudeArgs    &print_args,
    // and the object & support layer of the above.
    const ObjectLayerToPrint &layer_to_print
    )
{
    bool     first     = true;
    // Delay layer initialization as many layers may not print with all extruders.
    auto init_layer_delayed = [this, &print_args, &layer_to_print, &first]() {
        if (first) {
            first = false;
            const PrintObject &print_object = print_args.print_instance.print_object;
            const Print       &print        = *print_object.print();
            m_config.apply(print_object.config(), true);
            m_layer = layer_to_print.layer();
            m_print_object_instance_id = static_cast<uint16_t>(print_args.print_instance.instance_id);
            const PrintInstance &instance = print_object.instances()[print_args.print_instance.instance_id];
            if (print.config().avoid_crossing_perimeters)
                m_avoid_crossing_perimeters.init_layer(*m_layer);
            // ask for a bigger lift for travel to object when moving to another object
            if (m_last_instance == nullptr || (&instance != m_last_instance))
                set_extra_lift(m_last_layer_z, layer()->id(), print.config(), m_writer, print_args.extruder_id);
            m_last_instance = &instance;
            // When starting a new object, use the external motion planner for the first travel move.
            const Point &offset = instance.shift;
            GCode::PrintObjectInstance next_instance = {&print_object, int(print_args.print_instance.instance_id)};
            if (m_current_instance != next_instance)
                m_avoid_crossing_perimeters.use_external_mp_once();
            m_current_instance = next_instance;
            this->set_origin(unscale(offset));
            assert(!m_gcode_label_objects_in_session);
            m_gcode_label_objects_start = m_label_objects.start_object(instance, GCode::LabelObjects::IncludeName::No);
            
            if (!print_args.print_instance.print_object.config().object_gcode.value.empty()) {
                DynamicConfig config;
                config.set_key_value("layer_num", new ConfigOptionInt(m_layer_index));
                assert(std::abs(m_writer.get_position().z() - m_config.z_offset.value - m_last_layer_z) < 0.0001);
                config.set_key_value("layer_z",     new ConfigOptionFloat(m_last_layer_z));
                m_gcode_label_objects_start += this->placeholder_parser_process("object_gcode",
                    print_args.print_instance.print_object.config().object_gcode.value, m_writer.tool()->id(), &config)
                    + "\n";
            }
        }
    };

    const PrintObject &print_object = print_args.print_instance.print_object;
    const Print       &print        = *print_object.print();

    if (! print_args.print_wipe_extrusions && layer_to_print.support_layer != nullptr)
        if (const SupportLayer &support_layer = *layer_to_print.support_layer; ! support_layer.support_fills.entities().empty()) {
            ExtrusionRole   role               = support_layer.support_fills.role();
            bool            has_support        = role.is_mixed() || role.is_support_base();
            bool            has_interface      = role.is_mixed() || role.is_support_interface();
            // Extruder ID of the support base. -1 if "don't care".
            unsigned int    support_extruder   = print_object.config().support_material_extruder.value - 1;
            // Shall the support be printed with the active extruder, preferably with non-soluble, to avoid tool changes?
            bool            support_dontcare   = support_extruder == std::numeric_limits<unsigned int>::max();
            // Extruder ID of the support interface. -1 if "don't care".
            unsigned int    interface_extruder = print_object.config().support_material_interface_extruder.value - 1;
            // Shall the support interface be printed with the active extruder, preferably with non-soluble, to avoid tool changes?
            bool            interface_dontcare = interface_extruder == std::numeric_limits<unsigned int>::max();
            if (support_dontcare || interface_dontcare) {
                // Some support will be printed with "don't care" material, preferably non-soluble.
                // Is the current extruder assigned a soluble filament?
                auto it_nonsoluble = std::find_if(print_args.layer_tools.extruders.begin(), print_args.layer_tools.extruders.end(), 
                    [&soluble = std::as_const(print.config().filament_soluble), &print_args](unsigned int extruder_id) { return ! soluble.get_at(print_args.extruder_id); });
                // There should be a non-soluble extruder available.
                assert(it_nonsoluble != print_args.layer_tools.extruders.end());
                unsigned int dontcare_extruder = it_nonsoluble == print_args.layer_tools.extruders.end() ? print_args.layer_tools.extruders.front() : *it_nonsoluble;
                if (support_dontcare)
                    support_extruder = dontcare_extruder;
                if (interface_dontcare)
                    interface_extruder = dontcare_extruder;
            }
            bool extrude_support   = has_support && support_extruder == print_args.extruder_id;
            bool extrude_interface = has_interface && interface_extruder == print_args.extruder_id;
            if (extrude_support || extrude_interface) {
                init_layer_delayed();
                m_layer = layer_to_print.support_layer;
                m_object_layer_over_raft = false;
                ExtrusionEntitiesPtr        entities_cache;
                const ExtrusionEntitiesPtr &entities = extrude_support && extrude_interface ? support_layer.support_fills.entities() : entities_cache;
                if (! extrude_support || ! extrude_interface) {
                    auto role = extrude_support ? ExtrusionRole::SupportMaterial : ExtrusionRole::SupportMaterialInterface;
                    entities_cache.reserve(support_layer.support_fills.entities().size());
                    for (ExtrusionEntity *ee : support_layer.support_fills.entities())
                        if (ee->role() == role)
                            entities_cache.emplace_back(ee);
                }
                if (m_layer != nullptr && m_layer->bottom_z() < EPSILON && m_config.print_first_layer_temperature.value > 0)
                    gcode += m_writer.set_temperature(m_config.print_first_layer_temperature.value, false, m_writer.tool()->id());
                else if (m_config.print_temperature.value > 0)
                    gcode += m_writer.set_temperature(m_config.print_temperature.value, false, m_writer.tool()->id());
                else if (m_layer != nullptr && m_layer->bottom_z() < EPSILON && m_config.first_layer_temperature.get_at(m_writer.tool()->id()) > 0)
                        gcode += m_writer.set_temperature(m_config.first_layer_temperature.get_at(m_writer.tool()->id()), false, m_writer.tool()->id());
                else if (m_config.temperature.get_at(m_writer.tool()->id()) > 0) // don't set it if disabled
                    gcode += m_writer.set_temperature(m_config.temperature.get_at(m_writer.tool()->id()), false, m_writer.tool()->id());
                gcode += this->extrude_support(chain_extrusion_references(entities, last_pos_defined()?&last_pos():nullptr));
            }
        }
    m_layer = layer_to_print.layer();
    // To control print speed of the 1st object layer printed over raft interface.
    m_object_layer_over_raft = layer_to_print.object_layer && layer_to_print.object_layer->id() > 0 &&
        print_object.slicing_parameters().raft_layers() == layer_to_print.object_layer->id();

    //extrude instance-only brim
    bool print_object_skirtbrim_start = print.config().complete_objects.value || print.config().parallel_objects_step > 0;
    if (!print_object_skirtbrim_start && this->m_layer != nullptr && this->m_layer->id() == 0 && !print_args.print_instance.print_object.brim().empty()) {
        assert(print_args.print_instance.instance_id < print_args.print_instance.print_object.brim().items_count());
        Vec2d offset = this->origin(); 
        this->set_origin(0., 0.);
        if (this->m_layer != nullptr && this->m_layer->id() == 0) {
            m_avoid_crossing_perimeters.use_external_mp(true);
            assert(print_args.print_instance.print_object.brim().entities()[print_args.print_instance.instance_id]->is_collection());
            if (const ExtrusionEntityCollection *coll = dynamic_cast<const ExtrusionEntityCollection *>(
                    print_args.print_instance.print_object.brim().entities()[print_args.print_instance.instance_id])) {
                for (const ExtrusionEntity* ee : coll->entities())
                    gcode += this->extrude_entity(ExtrusionEntityReference{*ee, false}, "Brim"sv);
            }
            m_avoid_crossing_perimeters.use_external_mp(false);
            m_avoid_crossing_perimeters.disable_once();
        }
        this->set_origin(offset);
    }

    ExtrusionEntitiesPtr temp_fill_extrusions;
    if (const Layer *layer = layer_to_print.object_layer; layer) {
        for (size_t idx : layer->lslice_indices_sorted_by_print_order) {
            const LayerSlice &lslice = layer->lslices_ex[idx];

            //FIXME order islands?
            // Sequential tool path ordering of multiple parts within the same object, aka. perimeter tracking (#5511)
            for (const LayerIsland &island : lslice.islands) {
                init_layer_delayed();
                this->extrude_infill(print_args, island, true, gcode);
                this->extrude_perimeters(print_args, island, gcode);
                this->extrude_infill(print_args, island, false, gcode);
                this->extrude_ironing(print_args, island, gcode);

                //clear any leftover
                if(!m_last_too_small.empty()){
                    // finish extrude the little thing that was left before us and incompatible with our next extrusion.
                    ExtrusionPath to_finish = m_last_too_small;
                    gcode += this->_extrude(m_last_too_small, m_last_description, m_last_speed_mm_per_sec);
                    m_last_too_small.polyline.clear();
                }
            }
        }
    }
    // Don't set m_gcode_label_objects_end if you don't had to write the m_gcode_label_objects_start.
    // m_gcode_label_objects_start != "" => start_object has been called, but not written on gcode.
    // first == true => start_object has NOT been called
    if (!m_gcode_label_objects_start.empty()) {
        assert(!m_gcode_label_objects_in_session);
        m_gcode_label_objects_start = "";
    } else if (!first) {
        assert(m_gcode_label_objects_in_session);
        m_gcode_label_objects_end = m_label_objects.stop_object(print_args.print_instance.print_object.instances()[print_args.print_instance.instance_id]);
    } else {
        assert(!m_gcode_label_objects_in_session);
    }
}

void GCodeGenerator::emit_milling_commands(std::string& gcode, const ObjectsLayerToPrint& layers)
{
    //TODO: put post-process on their own thread.

    //add milling post-process if enabled
    if (!config().milling_diameter.empty()) {
        bool milling_ok = false;
        for (const ObjectLayerToPrint& ltp : layers) {
            if (ltp.object_layer != nullptr) {
                for (const LayerRegion* lr : ltp.object_layer->regions()) {
                    if (!lr->millings().empty()) {
                        milling_ok = true;
                        break;
                    }
                }
            }
        }
        if (milling_ok) {
            if (!m_gcode_label_objects_end.empty()) {
                gcode += m_gcode_label_objects_end;
                m_gcode_label_objects_end = "";
            }
            //switch to mill
            gcode += "; milling ok\n";
            uint32_t current_extruder_filament = m_writer.tool()->id();
            uint32_t milling_extruder_id = uint32_t(config().nozzle_diameter.size());
            m_writer.toolchange(milling_extruder_id);
            this->placeholder_parser().set("current_extruder", milling_extruder_id);
            // Append the filament start G-code.
            const std::string& start_mill_gcode = m_config.milling_toolchange_start_gcode.get_at(0);
            coordf_t previous_print_z = m_layer != nullptr ? m_layer->print_z : 0;
            if (!start_mill_gcode.empty()) {
                DynamicConfig config;
                config.set_key_value("previous_extruder", new ConfigOptionInt((int)current_extruder_filament));
                config.set_key_value("next_extruder", new ConfigOptionInt((int)milling_extruder_id));
                config.set_key_value("layer_num", new ConfigOptionInt(m_layer_index));
                config.set_key_value("previous_layer_z", new ConfigOptionFloat(previous_print_z));
                config.set_key_value("layer_z", new ConfigOptionFloat(m_layer->print_z));
                config.set_key_value("max_layer_z", new ConfigOptionFloat(m_max_layer_z));
                // Process the start_mill_gcode for the new filament.
                gcode += this->placeholder_parser_process("milling_toolchange_start_gcode", start_mill_gcode,
                                                          current_extruder_filament, &config);
                check_add_eol(gcode);
            }

            gcode += "\n; began milling:\n";
            for (const ObjectLayerToPrint& ltp : layers) {
                if (ltp.object_layer != nullptr) {
                    for (const PrintInstance& print_instance : ltp.object()->instances()) {
                        this->set_origin(unscale(print_instance.shift));
                        for (const LayerRegion* lr : ltp.object_layer->regions()) {
                            if (!lr->millings().empty()) {
                                //EXTRUDE MOVES
                                gcode += "; extrude lr->milling\n";
                                gcode += this->extrude_entity({lr->millings(), false}, "; milling post-process");
                            }
                        }
                    }
                }
            }

            //switch to extruder
            this->placeholder_parser().set("current_extruder", current_extruder_filament);
            // Append the filament start G-code.
            const std::string& end_mill_gcode = m_config.milling_toolchange_end_gcode.get_at(0);
            if (!end_mill_gcode.empty()) {
                DynamicConfig config;
                config.set_key_value("previous_extruder", new ConfigOptionInt((int)milling_extruder_id));
                config.set_key_value("next_extruder", new ConfigOptionInt((int)current_extruder_filament));
                config.set_key_value("layer_num", new ConfigOptionInt(m_layer_index));
                config.set_key_value("previous_layer_z", new ConfigOptionFloat(previous_print_z));
                config.set_key_value("layer_z", new ConfigOptionFloat(m_layer->print_z));
                config.set_key_value("max_layer_z", new ConfigOptionFloat(m_max_layer_z));
                // Process the end_mill_gcode for the new filament.
                gcode += this->placeholder_parser_process("milling_toolchange_start_gcode", end_mill_gcode,
                                                          current_extruder_filament, &config);
                check_add_eol(gcode);
            }
            gcode += "; will go back to normal extruder\n";
            m_writer.toolchange(current_extruder_filament);
            //TODO: change wipetower code to add an other filament change per layer.
            //gcode += (layer_tools.has_wipe_tower && m_wipe_tower) ?
            //    m_wipe_tower->tool_change(*this, current_extruder_filament, current_extruder_filament == layer_tools.extruders.back()) :
            //    this->set_extruder(current_extruder_filament, print_z);
        }
    }
}

// Check whether this ExtrusionEntityCollection should be printed now with extruder_id, given print_wipe_extrusions
// (wipe extrusions are printed before regular extrusions).
bool GCodeGenerator::shall_print_this_extrusion_collection(const ExtrudeArgs &print_args, const ExtrusionEntityCollection *eec, const PrintRegion &region)
{
//    [extruder_id, instance_id = print_instance.instance_id, &layer_tools, is_anything_overridden, print_wipe_extrusions] -> bool
    assert(eec != nullptr);
    if (eec->entities().empty()) {
        assert(false);
        // This shouldn't happen. FIXME why? but first_point() would fail.
        return false;
    }
    // This extrusion is part of certain Region, which tells us which extruder should be used for it:
    int correct_extruder_id = print_args.layer_tools.extruder(*eec, region);
    if (! print_args.layer_tools.has_extruder(correct_extruder_id)) {
        // this entity is not overridden, but its extruder is not in layer_tools - we'll print it
        // by last extruder on this layer (could happen e.g. when a wiping object is taller than others - dontcare extruders are eradicated from layer_tools)
        correct_extruder_id = print_args.layer_tools.extruders.back();
    }
    int extruder_override_id = print_args.is_anything_overridden ? print_args.layer_tools.wiping_extrusions().get_extruder_override(eec, print_args.print_instance.instance_id) : -1;
    return print_args.print_wipe_extrusions ?
        extruder_override_id == int(print_args.extruder_id) :
        extruder_override_id < 0 && int(print_args.extruder_id) == correct_extruder_id;
}

void GCodeGenerator::apply_print_configs(const Print &print)
{
    //apply also default region & object, just in case, as they won't be applied since the first extrusion on a region / the first layer process on an object
    m_writer.apply_print_config(print.config());
    m_writer.apply_print_region_config(print.default_region_config());
    m_config.apply(print.config());
    m_config.apply(print.default_object_config());
    m_config.apply(print.default_region_config());
    m_scaled_gcode_resolution = scale_d(print.config().gcode_resolution.value);
}

void GCodeGenerator::append_full_config(const Print& print, std::string &str)
{
    std::vector<std::pair<std::string, std::string>> config;
    encode_full_config(print, config);
    for (const auto& [key, value] : config) {
        str += "; " + key + " = " + value + "\n";
    }
}

void GCodeGenerator::encode_full_config(const Print& print, std::vector<std::pair<std::string, std::string>>& config)
{
    const DynamicPrintConfig &cfg = print.full_print_config();
    // Sorted list of config keys, which shall not be stored into the G-code. Initializer list.
    const std::vector<std::string_view> banned_keys { 
        "compatible_printers"sv,
        "compatible_prints"sv,
        //FIXME The print host keys should not be exported to full_print_config anymore. The following keys may likely be removed.
        "print_host"sv,
        "printhost_apikey"sv,
        "printhost_cafile"sv,
        "printhost_client_cert"sv,
        "printhost_client_cert_password"sv,
        "printhost_port"sv,
    };
    assert(std::is_sorted(banned_keys.begin(), banned_keys.end()));
    auto is_banned = [&banned_keys](const std::string &key) {
        return std::binary_search(banned_keys.begin(), banned_keys.end(), key);
    };
    config.reserve(config.size() + cfg.keys().size());
    for (const std::string& key : cfg.keys()) {
        if (!is_banned(key) && !cfg.option(key)->is_nil())
            config.emplace_back(key, cfg.opt_serialize(key));
    }
    config.shrink_to_fit();
}

void GCodeGenerator::set_extruders(const std::vector<uint16_t>& extruder_ids)
{
    m_writer.set_extruders(extruder_ids);

    // enable wipe path generation if any extruder has wipe enabled
    m_wipe.init(this->config(), this->m_writer, extruder_ids);
}

void GCodeGenerator::set_origin(const Vec2d &pointf)
{
    // if origin increases (goes towards right), last_pos decreases because it goes towards left
    const auto offset = Point::new_scale(m_origin - pointf);
    if (this->last_pos_defined())
        this->set_last_pos(this->last_pos() + offset);

    m_wipe.offset_path(offset);
    m_origin = pointf;
}

std::string GCodeGenerator::preamble()
{
    std::string gcode;
    
    if (!this->config().start_gcode_manual)
        gcode = m_writer.preamble();

    /*  Perform a *silent* move to z_offset: we need this to initialize the Z
        position of our writer object so that any initial lift taking place
        before the first layer change will raise the extruder from the correct
        initial Z instead of 0.  */
    m_writer.travel_to_z(m_config.z_offset.value);
    //as this phony thing skip the acceleration writing, they have to be reset after that for real initialisation at the next move/extrusion
    m_writer.set_acceleration(0);
    
    return gcode;
}

// called by GCodeGenerator::process_layer()
std::string GCodeGenerator::change_layer(
    coordf_t previous_layer_z,
    coordf_t print_z,
    bool vase_mode
) {
    std::string gcode;
    if (layer_count() > 0)
        // Increment a progress bar indicator.
        gcode += m_writer.update_progress(++ m_layer_index, layer_count());

    if (!BOOL_EXTRUDER_CONFIG(travel_ramping_lift) && BOOL_EXTRUDER_CONFIG(retract_layer_change)) {
        gcode += this->retract_and_wipe();
    } else if (BOOL_EXTRUDER_CONFIG(travel_ramping_lift) && !vase_mode){
        m_previous_layer_last_position_before_wipe = this->last_pos_defined() ?
            std::optional{to_3d(this->point_to_gcode(this->last_pos()), previous_layer_z)} :
            std::nullopt;
        gcode += GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Layer_Change_Retraction_Start);
        gcode += this->retract_and_wipe();
        gcode += GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Layer_Change_Retraction_End);
    }

    //if needed, write the gcode_label_objects_end then gcode_label_objects_start
    _add_object_change_labels(gcode);

    Vec3d new_position = this->writer().get_position();
    new_position.z() = print_z;
    this->writer().update_position(new_position);

    m_previous_layer_last_position = this->last_pos_defined() ?
        std::optional{to_3d(this->point_to_gcode(this->last_pos()), previous_layer_z)} :
        std::nullopt;

    gcode += GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Layer_Change_Travel);
    this->m_layer_change_extruder_id = m_writer.tool()->id();

    // forget last wiping path as wiping after raising Z is pointless
    m_wipe.reset_path();

    return gcode;
}

//TODO: rework: just change the core path extrusion to change z, and add extra loops in the middle.
//like extrude_loop but with varying z and two full round
std::string GCodeGenerator::extrude_loop_vase(const ExtrusionLoop &original_loop, const std::string_view description, double speed)
{
    //don't keep the speed
    speed = -1;
    // get a copy; don't modify the orientation of the original loop object otherwise
    // next copies (if any) would not detect the correct orientation
    ExtrusionLoop loop_to_seam = original_loop;
    bool save_flipped = this->visitor_flipped;
    if (this->visitor_flipped) {
        assert(loop_to_seam.can_reverse());
        loop_to_seam.reverse();
    }
    this->visitor_flipped = false;

    // extrude all loops ccw
    //no! this was decided in perimeter_generator
    bool is_hole_loop = (loop_to_seam.loop_role() & ExtrusionLoopRole::elrHole) != 0;// loop.make_counter_clockwise();
    bool reverse_turn = loop_to_seam.polygon().is_clockwise() ^ is_hole_loop;

    split_at_seam_pos(loop_to_seam, reverse_turn);
    const coordf_t full_loop_length = loop_to_seam.length();

    // don't clip the path ?
    ExtrusionPaths &paths = loop_to_seam.paths;
    if (false && m_enable_loop_clipping && m_writer.tool_is_extruder()) {
        coordf_t clip_length = scale_(m_config.seam_gap.get_abs_value(m_writer.tool()->id(), EXTRUDER_CONFIG_WITH_DEFAULT(nozzle_diameter, 0)));
        if (original_loop.role() == ExtrusionRole::ExternalPerimeter) {
            coordf_t clip_length_external = scale_(m_config.seam_gap_external.get_abs_value(m_writer.tool()->id(), unscaled(clip_length)));
            if (clip_length_external > 0) {
                clip_length = clip_length_external;
            }
        }
        coordf_t min_clip_length = scale_(EXTRUDER_CONFIG_WITH_DEFAULT(nozzle_diameter, 0)) * 0.15;

        // get paths
        ExtrusionPaths clipped;
        if (clip_length > min_clip_length) {
            clipped = clip_end(paths, clip_length);
            clip_end(clipped, min_clip_length);
            for (ExtrusionPath& ep : clipped)
                ep.attributes_mutable().mm3_per_mm = 0;
            append(paths, clipped);
        } else {
            clip_end(paths, clip_length);
        }
    }

    if (paths.empty()) return "";

    // apply the small/external? perimeter speed
    if (speed == -1 && paths.front().role().is_perimeter() && this->m_config.small_perimeter_speed.value > 0 && paths.front().role() != ExtrusionRole::ThinWall){
        coordf_t min_length = scale_d(this->m_config.small_perimeter_min_length.get_abs_value(EXTRUDER_CONFIG_WITH_DEFAULT(nozzle_diameter, 0)));
        coordf_t max_length = scale_d(this->m_config.small_perimeter_max_length.get_abs_value(EXTRUDER_CONFIG_WITH_DEFAULT(nozzle_diameter, 0)));
        max_length = std::max(min_length, max_length);
        if (full_loop_length < max_length) {
            if (full_loop_length <= min_length) {
                speed = SMALL_PERIMETER_SPEED_RATIO_OFFSET;
            } else if (max_length > min_length) {
                //use a negative speed: it will be use as a ratio when computing the real speed
                speed = SMALL_PERIMETER_SPEED_RATIO_OFFSET - (full_loop_length - min_length) / (max_length - min_length);
            }
        }
    }

    //get extrusion length
    coordf_t length = 0;
    for (ExtrusionPaths::iterator path = paths.begin(); path != paths.end(); ++path) {
        //path->simplify(SCALED_RESOLUTION); //not useful, this should have been done before.
        length += path->length() * SCALING_FACTOR;
    }

    //all in unscaled coordinates (hence why it's coordf_t and not coord_t)
    const coordf_t min_height = m_config.min_layer_height.get_abs_value(m_writer.tool()->id(), m_config.nozzle_diameter.get_at(m_writer.tool()->id()));
    const coordf_t bot_init_z = - this->m_layer->height;
    //const coordf_t bot_last_z = bot_init_z + this->m_layer->height - m_config.min_layer_height.get_abs_value(m_writer.tool()->id(), m_config.nozzle_diameter.get_at(m_writer.tool()->id()));
    const coordf_t init_z = bot_init_z + min_height;
    //const coordf_t last_z = bot_init_z + this->m_layer->height;

    Point inward_point;
    //move the seam point inward a little bit
    if (EXTRUDER_CONFIG_WITH_DEFAULT(wipe_inside_end, true) && paths.back().role() == ExtrusionRole::ExternalPerimeter && m_layer != NULL &&
        m_config.perimeters.value > 1 && paths.front().size() >= 2 && paths.back().polyline.size() >= 3) {
        // detect angle between last and first segment
        // the side depends on the original winding order of the polygon (left for contours, right for holes)
        //FIXME improve the algorithm in case the loop is tiny.
        //FIXME improve the algorithm in case the loop is split into segments with a low number of points (see the Point b query).
        Point a = paths.front().polyline.get_point(1);  // second point
        Point b = paths.back().polyline.get_point(paths.back().polyline.size() - 2);       // second to last point
        if (reverse_turn) {
            // swap points
            Point c = a; a = b; b = c;
        }
        assert(ccw_angle_old_test(paths.front().first_point(), a, b) ==
               abs_angle(angle_ccw( a - paths.front().first_point(),b - paths.front().first_point())));
        double angle = abs_angle(angle_ccw(a-paths.front().first_point(),b-paths.front().first_point())) * 2 / 3;

        // turn left if contour, turn right if hole
        if (reverse_turn) angle *= -1;

        // create the destination point along the first segment and rotate it
        // we make sure we don't exceed the segment length because we don't know
        // the rotation of the second segment so we might cross the object boundary
        Vec2d  p1 = paths.front().polyline.front().cast<double>();
        Vec2d  p2 = paths.front().polyline.get_point(1).cast<double>();
        Vec2d  v = p2 - p1;
        double nd = scale_d(EXTRUDER_CONFIG_WITH_DEFAULT(nozzle_diameter, paths.front().width()));
        double l2 = v.squaredNorm();
        // Shift by no more than a nozzle diameter.
        //FIXME Hiding the seams will not work nicely for very densely discretized contours!
        inward_point = (/*(nd * nd >= l2) ? p2 : */(p1 + v * (nd / sqrt(l2)))).cast<coord_t>();
        inward_point.rotate(angle, paths.front().polyline.front());
    }

    coordf_t current_pos_in_length = 0;
    coordf_t current_z = 0; // over init_z
    coordf_t current_height = min_height;
    coordf_t starting_height = min_height;
    enum Step {
        INCR = 0,
        FLAT = 1
    };
    std::string gcode;
    for (int step = 0; step < 2; step++) {
        current_pos_in_length = 0;
        current_z = 0;
        const coordf_t z_per_length = (step == Step::INCR) ? ((this->m_layer->height - (min_height + min_height)) / length) : 0;
        const coordf_t height_per_length = (step == Step::INCR) ? ((this->m_layer->height- (min_height + min_height)) / length) : ((-this->m_layer->height + (min_height + min_height)) / length);
        if (step == Step::FLAT) {
            current_height = this->m_layer->height - min_height;
            starting_height = this->m_layer->height - min_height;
        }
        Vec3d previous;
        for (ExtrusionPaths::iterator path = paths.begin(); path != paths.end(); ++path) {
            if (path == paths.begin() ){
                if (step == Step::INCR) {
                    if (paths.back().role() == ExtrusionRole::ExternalPerimeter && m_layer != NULL && m_config.perimeters.value > 1 && paths.front().size() >= 2 && paths.back().polyline.size() >= 3) {
                        paths[0].polyline.append_before(inward_point);
                    }
                    this->m_writer.travel_to_z(this->m_layer->print_z + init_z);
                } else {
                    //ensure we're at the right height
                    this->m_writer.travel_to_z(this->m_layer->print_z);
                }
            }
            gcode += this->_before_extrude(*path, description, speed);
            if (path == paths.begin() && step == Step::INCR){
                if (paths.back().role() == ExtrusionRole::ExternalPerimeter && m_layer != NULL && m_config.perimeters.value > 1 && paths.front().size() >= 2 && paths.back().polyline.size() >= 3) {
                    assert(! paths.empty() && paths.front().polyline.size() > 1 && !paths.front().polyline.get_arc(1).linear());
                    paths[0].polyline.pop_front();
                    gcode += m_writer.extrude_to_xy(this->point_to_gcode(paths[0].polyline.front()), 0);
                }
            }

            // calculate extrusion length per distance unit
            double e_per_mm_per_height = _compute_e_per_mm(path->mm3_per_mm());
            //extrude
            {
                std::string_view comment = config().gcode_comments ? description : ""sv;
                Polyline poly = path->polyline.to_polyline();
                for (const Line &line : poly.lines()) {
                    const coordf_t line_length = line.length() * SCALING_FACTOR;
                    //don't go (much) more than a nozzle_size without a refresh of the z & extrusion rate
                    const int nb_sections = std::max(1,int(line_length / EXTRUDER_CONFIG_WITH_DEFAULT(nozzle_diameter, path->width())));
                    const coordf_t height_increment = height_per_length * line_length / nb_sections;
                    Vec3d last_point{ this->point_to_gcode(line.a).x(), this->point_to_gcode(line.a).y(), current_z };
                    const Vec3d pos_increment{ (this->point_to_gcode(line.b).x() - last_point.x()) / nb_sections,
                        (this->point_to_gcode(line.b).y() - last_point.y()) / nb_sections,
                        z_per_length * line_length / nb_sections };
                    coordf_t current_height_internal = current_height + height_increment / 2;
                    //ensure you go to the good xyz
                    if( (last_point - previous).norm() > EPSILON)
                        gcode += m_writer.extrude_to_xyz(last_point, 0, description);
                    //extrusions
                    for (int i = 0; i < nb_sections - 1; i++) {
                        Vec3d new_point = last_point + pos_increment;
                        gcode += m_writer.extrude_to_xyz(new_point,
                            e_per_mm_per_height * (line_length / nb_sections) * current_height_internal,
                            description);
                        current_height_internal += height_increment;
                        last_point = new_point;
                    }
                    //last bit will go to the exact last pos
                    last_point.x() = this->point_to_gcode(line.b).x();
                    last_point.y() = this->point_to_gcode(line.b).y();
                    last_point.z() = current_z + z_per_length * line_length;
                    gcode += m_writer.extrude_to_xyz(
                        last_point,
                        e_per_mm_per_height * (line_length / nb_sections) * current_height_internal,
                        comment);
                    previous = last_point;

                    //update vars for next line
                    current_pos_in_length += line_length;
                    current_z = current_pos_in_length * z_per_length;//last_point.z();
                    current_height = starting_height + current_pos_in_length * height_per_length;
                }
            }
            gcode += this->_after_extrude(*path);
        }
    }

    // reset acceleration
    m_writer.set_acceleration((uint16_t)floor(get_default_acceleration(m_config) + 0.5));

    //don't wipe here
    //if (m_wipe.is_enabled())
    //    m_wipe.path = paths.front().polyline;  // TODO: don't limit wipe to last path

    //just continue on the perimeter a bit while retracting
    //FIXME this doesn't work work, hence why it's commented
    //coordf_t travel_length = std::min(length, EXTRUDER_CONFIG(nozzle_diameter) * 10);
    //for (auto & path : paths){
    //    for (const Line &line : path.polyline.lines()) {
    //        if (unscaled(line.length()) > travel_length) {
    //            // generate the travel move
    //            gcode += m_writer.travel_to_xy(this->point_to_gcode(line.b), 0.0, "move inwards before travel");
    //            travel_length -= unscaled(line.length());
    //        }
    //        else
    //        {
    //            gcode += m_writer.travel_to_xy(this->point_to_gcode(line.a) + (this->point_to_gcode(line.b) - this->point_to_gcode(line.a)) * (travel_length / unscaled(line.length())), 0.0, "move before travel");
    //            travel_length = 0;
    //            //double break;
    //            goto FINISH_MOVE;
    //        }
    //    }
    //}
    //FINISH_MOVE:

    // make a little move inwards before leaving loop
    if (paths.back().role() == ExtrusionRole::ExternalPerimeter && m_layer != NULL && m_config.perimeters.value > 1 && paths.front().size() >= 2 && paths.back().polyline.size() >= 3) {
        // detect angle between last and first segment
        // the side depends on the original winding order of the polygon (left for contours, right for holes)
        //FIXME improve the algorithm in case the loop is tiny.
        //FIXME improve the algorithm in case the loop is split into segments with a low number of points (see the Point b query).
        Point a = paths.front().polyline.get_point(1);  // second point
        Point b = paths.back().polyline.get_point(paths.back().polyline.size()-2);       // second to last point
        if (reverse_turn) {
            // swap points
            Point c = a; a = b; b = c;
        }
        assert(ccw_angle_old_test(paths.front().first_point(), a, b) == abs_angle(angle_ccw( a - paths.front().first_point(),b - paths.front().first_point())));
        double angle = abs_angle(angle_ccw( a - paths.front().first_point(),b - paths.front().first_point())) / 3;

        // turn left if contour, turn right if hole
        if (reverse_turn) angle *= -1;

        // create the destination point along the first segment and rotate it
        // we make sure we don't exceed the segment length because we don't know
        // the rotation of the second segment so we might cross the object boundary
        Vec2d  p1 = paths.front().polyline.front().cast<double>();
        Vec2d  p2 = paths.front().polyline.get_point(1).cast<double>();
        Vec2d  v = p2 - p1;
        coordf_t nd = scale_d(EXTRUDER_CONFIG_WITH_DEFAULT(nozzle_diameter, paths.front().width()));
        double l2 = v.squaredNorm();
        // Shift by no more than a nozzle diameter.
        //FIXME Hiding the seams will not work nicely for very densely discretized contours!
        inward_point = (/*(nd * nd >= l2) ? p2 : */(p1 + v * (nd / sqrt(l2)))).cast<coord_t>();
        inward_point.rotate(angle, paths.front().polyline.front());
        
        // generate the travel move
        gcode += m_writer.travel_to_xy(this->point_to_gcode(inward_point), 0.0, "move inwards before travel");
    }

    assert(!this->visitor_flipped);
    this->visitor_flipped = save_flipped;
    return gcode;
}

void GCodeGenerator::split_at_seam_pos(ExtrusionLoop& loop, bool was_clockwise)
{
    if (loop.paths.empty())
        return;

#if _DEBUG
    ExtrusionLoop old_loop = loop;
    for (const ExtrusionPath &path : loop.paths)
        for (int i = 1; i < path.polyline.size(); ++i)
            assert(!path.polyline.get_point(i - 1).coincides_with_epsilon(path.polyline.get_point(i)));
    for (auto it = std::next(loop.paths.begin()); it != loop.paths.end(); ++it) {
        assert(it->polyline.size() >= 2);
        assert(std::prev(it)->polyline.back() == it->polyline.front());
    }
    assert(loop.first_point() == loop.last_point());
#endif

//    SeamPosition seam_position = m_config.seam_position;
//    if (loop.loop_role() == elrSkirt)
//        seam_position = spNearest;
    
#if _DEBUG
    for (const ExtrusionPath &path : loop.paths)
        for (int i = 1; i < path.polyline.size(); ++i)
            assert(!path.polyline.get_point(i - 1).coincides_with_epsilon(path.polyline.get_point(i)));
    for (auto it = std::next(loop.paths.begin()); it != loop.paths.end(); ++it) {
        assert(it->polyline.size() >= 2);
        assert(std::prev(it)->polyline.back() == it->polyline.front());
    }
    assert(loop.first_point() == loop.last_point());
#endif

    // find the point of the loop that is closest to the current extruder position
    // or randomize if requested
    Point seam_point = this->last_pos_defined() ? this->last_pos() : Point(0,0);
    //for first spiral, choose the seam, as the position will be very relevant.
    if (m_spiral_vase_layer > 1 /* spiral vase is printing and it's after the transition layer (that one can find a good spot)*/
        || !m_seam_perimeters) {
        // Because the G-code export has 1um resolution, don't generate segments shorter than "1.5 microns" (depends of gcode_precision_xyz)
        coordf_t precision = pow(10, -m_config.gcode_precision_xyz.value) * 1.5;
        precision = std::max(precision, coordf_t(SCALED_EPSILON * 10));
        loop.split_at(seam_point, false, scale_t(precision));
    } else {
        assert(m_layer != nullptr);
        //FIXME update external_perimeters_first
        seam_point = m_seam_placer.place_seam(m_layer, loop,
            /*m_config.external_perimeters_first,*/
            m_print_object_instance_id,
            seam_point
            //EXTRUDER_CONFIG_WITH_DEFAULT(nozzle_diameter, 0.4),
            //m_print_object_instance_id,
            //lower_layer_edge_grid ? lower_layer_edge_grid->get() : nullptr
            );
        // Because the G-code export has 1um resolution, don't generate segments shorter than "1.5 microns" (depends of gcode_precision_xyz)
        if (!loop.split_at_vertex(seam_point, scaled<double>(0.0015))) {
            
#if _DEBUG
    for (const ExtrusionPath &path : loop.paths)
        for (int i = 1; i < path.polyline.size(); ++i)
            assert(!path.polyline.get_point(i - 1).coincides_with_epsilon(path.polyline.get_point(i)));
    for (auto it = std::next(loop.paths.begin()); it != loop.paths.end(); ++it) {
        assert(it->polyline.size() >= 2);
        assert(std::prev(it)->polyline.back() == it->polyline.front());
    }
    assert(loop.first_point() == loop.last_point());
#endif
            double precision = pow(10, -m_config.gcode_precision_xyz.value);
            precision *= 1.5;
            auto old_loop = loop;
            loop.split_at(seam_point, true, scale_t(precision));
            
#if _DEBUG
    for (const ExtrusionPath &path : loop.paths)
        for (int i = 1; i < path.polyline.size(); ++i)
            assert(!path.polyline.get_point(i - 1).coincides_with_epsilon(path.polyline.get_point(i)));
    for (auto it = std::next(loop.paths.begin()); it != loop.paths.end(); ++it) {
        assert(it->polyline.size() >= 2);
        assert(std::prev(it)->polyline.back() == it->polyline.front());
    }
    assert(loop.first_point() == loop.last_point());
#endif
    
            old_loop.split_at(seam_point, true, scale_t(precision));
        }
        
#if _DEBUG
    for (const ExtrusionPath &path : loop.paths)
        for (int i = 1; i < path.polyline.size(); ++i)
            assert(!path.polyline.get_point(i - 1).coincides_with_epsilon(path.polyline.get_point(i)));
    for (auto it = std::next(loop.paths.begin()); it != loop.paths.end(); ++it) {
        assert(it->polyline.size() >= 2);
        assert(std::prev(it)->polyline.back() == it->polyline.front());
    }
    assert(loop.first_point() == loop.last_point());
#endif
    }
#if _DEBUG
    for (const ExtrusionPath &path : loop.paths)
        for (int i = 1; i < path.polyline.size(); ++i)
            assert(!path.polyline.get_point(i - 1).coincides_with_epsilon(path.polyline.get_point(i)));
    for (auto it = std::next(loop.paths.begin()); it != loop.paths.end(); ++it) {
        assert(it->polyline.size() >= 2);
        assert(std::prev(it)->polyline.back() == it->polyline.front());
    }
    assert(loop.first_point() == loop.last_point());
#endif
}

namespace check_wipe {
    bool check_reduce(const Polygon& external_polygon, coord_t wipe_inside_depth, Point reference, coord_t threshold) {
        //reduce
        Polygons reduced = offset(external_polygon, -wipe_inside_depth);
        for (Polygon& poly_to_test : reduced) {
            const Point* closest = poly_to_test.closest_point(external_polygon.front());
            // because sqrt(2) = 1.42 and it's the worst case
            if (closest->distance_to(external_polygon.front()) < coordf_t(wipe_inside_depth * 1.43)) {
                Point pt_proj = poly_to_test.point_projection(reference).first;
                Point pt_temp;
                if (pt_proj.distance_to(reference) < threshold || poly_to_test.intersection(Line{ reference , external_polygon.front() }, &pt_temp)) {
                    //ok
                    return true;
                }
            }
        }
        return false;
    }
    coord_t max_depth(const ExtrusionPaths& paths, coord_t wipe_inside_depth, coord_t nozzle_width, std::function<Point(coord_t)> compute_point) {
        assert(wipe_inside_depth > 0);
        assert(nozzle_width > 0);
        //create polygon
        Polyline full_polyline;
        for (const ExtrusionPath& path : paths) {
            Polyline poly_temp = path.as_polyline().to_polyline(nozzle_width);
            full_polyline.points.insert(full_polyline.end(),poly_temp.begin(), poly_temp.end());
        }
        full_polyline.simplify(nozzle_width);
        Polygon external_polygon(full_polyline.points);
        // check dichotomic
        coord_t current_dist = wipe_inside_depth;
        bool increased = true;
        coord_t delta = wipe_inside_depth - nozzle_width / 2;
        bool success = check_reduce(external_polygon, current_dist, compute_point(current_dist), nozzle_width / 2);
        if (!success) {
            int iter = 0;
            const size_t nb_iter = size_t(std::sqrt(int(delta / nozzle_width)));
            for (iter = 0; iter < nb_iter; ++iter) {
                delta /= 2;
                increased = success;
                if (increased)
                    current_dist += delta;
                else
                    current_dist -= delta;
                success = check_reduce(external_polygon, current_dist, compute_point(current_dist), nozzle_width/2);
            }
        }
        if (!success) {
            return std::max(nozzle_width / 2, current_dist - delta);
        } else {
            return current_dist;
        }
    }
}

void GCodeGenerator::seam_notch(const ExtrusionLoop& original_loop,
    ExtrusionPaths& building_paths,
    ExtrusionPaths& notch_extrusion_start,
    ExtrusionPaths& notch_extrusion_end,
    bool is_hole_loop,
    bool is_full_loop_ccw) {
    assert(notch_extrusion_start.empty());
    assert(notch_extrusion_end.empty());
    assert(!building_paths.empty());
    if (original_loop.role() == ExtrusionRole::ExternalPerimeter && building_paths.front().size() > 1 && building_paths.back().size() > 1
        && (this->m_config.seam_notch_all.get_abs_value(1.) > 0 || this->m_config.seam_notch_inner.get_abs_value(1.) > 0 || this->m_config.seam_notch_outer.get_abs_value(1.) > 0)) {
        //TODO: check there is at least 4 points
        coord_t notch_value = 0;
        //check if applicable to seam_notch_inner
        if ((is_hole_loop && this->m_config.seam_notch_inner.get_abs_value(1.) > 0)
            || (!is_hole_loop && this->m_config.seam_notch_outer.get_abs_value(1.) > 0)
            ) {
            Polygon polygon_to_test = original_loop.polygon();
            if (polygon_to_test.size() > 8) {
                //check if the path is quasi-convex (~= as PrintObject::_transform_hole_to_polyholes)
                bool is_convex = false;
                if (is_hole_loop) {
                    //test if convex (as it's clockwise bc it's a hole, we have to do the opposite)
                    // 3.07 instead of PI to allow for some convex outliers (sometimes, stl can be a bit imprecise)
                    is_convex = polygon_to_test.convex_points(3.07).empty();
                } else {
                    // 3.3 instead of PI to allow for some concave outliers (sometimes, stl can be a bit imprecise)
                    is_convex = polygon_to_test.concave_points(3.3).empty();
                }
                if (is_convex) {
                    // Computing circle center
                    Point center = polygon_to_test.centroid();
                    double diameter_min = std::numeric_limits<float>::max(), diameter_max = 0;
                    double diameter_sum = 0;
                    for (int i = 0; i < polygon_to_test.points.size(); ++i) {
                        double dist = polygon_to_test.points[i].distance_to(center);
                        diameter_min = std::min(diameter_min, dist);
                        diameter_max = std::max(diameter_max, dist);
                        diameter_sum += dist;
                    }
                    //also use center of lines to check it's not a rectangle
                    double diameter_line_min = std::numeric_limits<float>::max(), diameter_line_max = 0;
                    Lines hole_lines = polygon_to_test.lines();
                    for (Line l : hole_lines) {
                        Point midline = (l.a + l.b) / 2;
                        double dist = center.distance_to(midline);
                        diameter_line_min = std::min(diameter_line_min, dist);
                        diameter_line_max = std::max(diameter_line_max, dist);
                    }
                    // allow flat ellipse up to 10*
                    coord_t max_variation = std::max(SCALED_EPSILON, scale_t(10 * (unscaled(diameter_sum / polygon_to_test.size()))));
                    if (diameter_max - diameter_min < max_variation * 2 && diameter_line_max - diameter_line_min < max_variation * 2) {
                        if (is_hole_loop) {
                            notch_value = scale_t(this->m_config.seam_notch_inner.get_abs_value(building_paths.front().width()));
                        } else {
                            notch_value = scale_t(this->m_config.seam_notch_outer.get_abs_value(building_paths.front().width()));
                        }
                    }
                }
            }
        }
        if (notch_value == 0) {
            notch_value = scale_t(this->m_config.seam_notch_all.get_abs_value(building_paths.front().width()));
        }
        if (notch_value == 0) {
            return;
        }

        const coordf_t notch_length = notch_value * 2;

        //check the loop is long enough
        if (original_loop.length() < notch_value * 4) {
            return;
        }
        //ensure it doesn't go too far
        notch_value = std::min(notch_value, scale_t(building_paths.front().width()) / 2);

        // found a suitable path, move the seam inner
        
        // extract paths from the start
        coordf_t dist = notch_length;
        while (dist > 0) {
            coordf_t length = building_paths.front().as_polyline().length();
            if(length > dist){
                //found the place to split
                notch_extrusion_start.emplace_back(building_paths.front().attributes(), building_paths.front().can_reverse());
                ArcPolyline ap2;
                building_paths.front().as_polyline().split_at(dist, notch_extrusion_start.back().polyline, ap2);
                building_paths.front().polyline = ap2;
                dist = 0;
            }else{
                notch_extrusion_start.push_back(std::move(building_paths.front()));
                building_paths.erase(building_paths.begin());
                dist -= length;
            }
        }
        // extract paths from the end
        dist = notch_length;
        while (dist > 0) {
            coordf_t length = building_paths.back().as_polyline().length();
            if(length > dist){
                //found the place to split
                notch_extrusion_end.emplace_back(building_paths.back().attributes(), building_paths.back().can_reverse());
                ArcPolyline ap1;
                building_paths.back().as_polyline().split_at(dist, ap1, notch_extrusion_end.back().polyline);
                building_paths.back().polyline = ap1;
                dist = 0;
            }else{
                notch_extrusion_end.push_back(std::move(building_paths.back()));
                building_paths.erase(building_paths.begin());
                dist -= length;
            }
            //notch_extrusion_end has benn created "in-reverse", I have to put it the right way
            std::reverse(notch_extrusion_end.begin(), notch_extrusion_end.end());
        }
        
        //kind of the same as the wipe
        Point prev_point = notch_extrusion_end.back().first_point();       // second to last point
        Point end_point = notch_extrusion_end.back().last_point();
        Point start_point = notch_extrusion_start.front().first_point();
        Point next_point = notch_extrusion_start.front().last_point();  // second point
        //safeguard : if a ExtrusionRole::ror exist abord;
        if (next_point == start_point || prev_point == end_point) {
            throw Slic3r::SlicingError(_u8L("ExtrusionRole::ror while writing gcode: two points are at the same position. Please send the .3mf project to the dev team for debugging. Extrude loop: seam notch."));
        }
        if(building_paths.size() == 1)
            assert(is_full_loop_ccw == Polygon(building_paths.front().polyline.to_polyline().points).is_counter_clockwise());
        double angle = PI / 2;
        if (is_hole_loop ? (is_full_loop_ccw) : (!is_full_loop_ccw)) {
            angle *= -1;
        }
        Vec2d  vec_start = next_point.cast<double>() - start_point.cast<double>();
        vec_start.normalize();
        Vec2d  vec_end = end_point.cast<double>() - prev_point.cast<double>();
        vec_end.normalize();
        //abord if the two vec are too different
        double prod_scal = vec_start.dot(vec_end);
        if (prod_scal < 0.2) {
            BOOST_LOG_TRIVIAL(warning) << "notch abord: too different sides\n";
            return;
        }
        //use a vec that is the mean between the two.
        vec_start = (vec_start + vec_end) / 2;

        Point moved_start = (start_point.cast<double>() + vec_start * notch_value).cast<coord_t>();
        moved_start.rotate(angle, start_point);
        Point moved_end = (end_point.cast<double>() + vec_start * notch_value).cast<coord_t>();
        moved_end.rotate(angle, end_point);

        //check if the current angle isn't too sharp
        double check_angle = 0;
        // get min angle (and if at min or max value, push it a bit more to avoid not filtering outliers)
        double min_angle = this->m_config.seam_notch_angle.value;
        if(min_angle <= 179.9) min_angle -= 1;
        if(min_angle >= 359.9) min_angle += 1;
        min_angle *= PI / 180.;
        if (end_point.distance_to_square(start_point) < SCALED_EPSILON * SCALED_EPSILON) {
            assert(ccw_angle_old_test(start_point, prev_point, next_point) == abs_angle(angle_ccw( prev_point -start_point,next_point- start_point)));
            check_angle = abs_angle(angle_ccw( prev_point -start_point,next_point- start_point));
        } else {
            assert(ccw_angle_old_test(end_point, prev_point, start_point) == abs_angle(angle_ccw( start_point -end_point,prev_point- end_point)));
            check_angle = abs_angle(angle_ccw( start_point -end_point,prev_point- end_point));
            if ((is_hole_loop ? -check_angle : check_angle) > min_angle) {
                BOOST_LOG_TRIVIAL(debug) << "notch abord: too big angle\n";
                return;
            }
            assert(ccw_angle_old_test(start_point, end_point, next_point) == abs_angle(angle_ccw( end_point - start_point,next_point - start_point)));
            check_angle = abs_angle(angle_ccw( end_point - start_point,next_point - start_point));
        }
        assert(end_point != start_point);
        assert(end_point != next_point);
        if ((is_hole_loop ? -check_angle : check_angle) > min_angle) {
            BOOST_LOG_TRIVIAL(debug) << "notch abord: too big angle\n";
            return;
        }
        //check if the point is inside
        bool is_inside = original_loop.polygon().contains(moved_start) && original_loop.polygon().contains(moved_end);
        if ( (is_hole_loop && is_inside) || (!is_hole_loop && !is_inside) ) {
            BOOST_LOG_TRIVIAL(debug) << "notch abord: not inside\n";
            return;
        }
        auto create_new_extrusion = [](ExtrusionPaths& paths, const ExtrusionPath& model, float ratio, const Point& start, const Point& end) {
            // add notch extrutsions
            paths.emplace_back(model);
            ExtrusionPath& path = paths.back();
            path.polyline.clear();
            path.polyline.append(start);
            path.polyline.append(end);
            //reduce the flow of the notch path, as it's longer than previously
            path.attributes_mutable().width = path.width() * ratio;
            path.attributes_mutable().mm3_per_mm = path.mm3_per_mm() * ratio;
        };

        //TODO change below things to avoid deleting more than one path, or at least preserve their flow
        
        //reduce the flow of the notch path, as it's longer than previously
        // test if the path isn't too curved/sharp
        coordf_t length_temp = notch_length;
        if (length_temp * length_temp < 
            1.4 * notch_extrusion_start.front().polyline.front().distance_to_square(notch_extrusion_start.back().polyline.back())) {
            //create a gentle curve
            Point p1 = Line(moved_start, Line(start_point, notch_extrusion_start.front().polyline.front()).midpoint()).midpoint();
            Point p2 = Line(p1, building_paths.front().first_point()).midpoint();
            p2 = Line(p2, notch_extrusion_start.back().polyline.back()).midpoint();
            ExtrusionPath model = notch_extrusion_start.front();
            model.polyline.clear();
            notch_extrusion_start.clear();
            create_new_extrusion(notch_extrusion_start, model, 0.25f, moved_start, p1);
            create_new_extrusion(notch_extrusion_start, model, 0.5f, p1, p2);
            create_new_extrusion(notch_extrusion_start, model, 0.75f, p2, building_paths.front().first_point());
        } //else : keep the path as-is
        //reduce the flow of the notch path, as it's longer than previously
        length_temp = notch_length;
        if (length_temp * length_temp < 
            1.4 * notch_extrusion_end.front().polyline.front().distance_to_square(notch_extrusion_end.back().polyline.back())) {
            //create a gentle curve
            Point p1 = Line(moved_end, notch_extrusion_end.front().polyline.front()).midpoint();
            Point p2 = Line(p1, building_paths.back().last_point()).midpoint();
            p2 = Line(p2, notch_extrusion_end.front().polyline.front()).midpoint();
            float flow_ratio = 0.75f;
            auto check_length_clipped = [&building_paths, &end_point, notch_length](const Point& pt_to_check) {
                if (notch_length > 0) {
                    double dist = pt_to_check.projection_onto(building_paths.back().last_point(), end_point).distance_to(end_point);
                    if (dist < notch_length) {
                        return false;
                    }
                }
                return true;
            };
            ExtrusionPath model = notch_extrusion_end.front();
            model.polyline.clear();
            notch_extrusion_end.clear();
            create_new_extrusion(notch_extrusion_end, model, check_length_clipped(p2)?0.75f:0.f, building_paths.back().last_point(), p2);
            create_new_extrusion(notch_extrusion_end, model, check_length_clipped(p1) ? 0.5f : 0.f, p2, p1);
            create_new_extrusion(notch_extrusion_end, model, 0.f, p1, moved_end);
        } //else : keep the path as-is
    }
}

std::string GCodeGenerator::extrude_loop(const ExtrusionLoop &original_loop, const std::string_view description, double speed)
{
#if _DEBUG
    for (auto it = std::next(original_loop.paths.begin()); it != original_loop.paths.end(); ++it) {
        assert(it->polyline.size() >= 2);
        assert(std::prev(it)->polyline.back() == it->polyline.front());
    }
    assert(original_loop.paths.front().first_point() == original_loop.paths.back().last_point());
#endif
#if DEBUG_EXTRUSION_OUTPUT
    std::cout << "extrude loop_" << (original_loop.polygon().is_counter_clockwise() ? "ccw" : "clw") << ": ";
    for (const ExtrusionPath &path : original_loop.paths) {
        std::cout << ", path{ ";
        for (const Point &pt : path.polyline.get_points()) {
            std::cout << ", " << floor(100 * unscale<double>(pt.x())) / 100.0 << ":" << floor(100 * unscale<double>(pt.y())) / 100.0;
        }
        std::cout << "}";
    }
    std::cout << "\n";
#endif

    //useful var
    const double nozzle_diam = (EXTRUDER_CONFIG_WITH_DEFAULT(nozzle_diameter, 0));

    //no-seam code path redirect
    if (original_loop.role() == ExtrusionRole::ExternalPerimeter && (original_loop.loop_role() & elrVase) != 0 && !this->m_config.spiral_vase
        //but not for the first layer
        && this->m_layer->id() > 0
        //exclude if min_layer_height * 2 > layer_height (increase from 2 to 3 because it's working but uses in-between)
        && this->m_layer->height >= m_config.min_layer_height.get_abs_value(m_writer.tool()->id(), nozzle_diam) * 2 - EPSILON) {
        return extrude_loop_vase(original_loop, description, speed);
    }

    ExtrusionLoop loop_to_seam = original_loop;
    for (const ExtrusionPath &path : loop_to_seam.paths)
        for (int i = 1; i < path.polyline.size(); ++i)
            assert(!path.polyline.get_point(i - 1).coincides_with_epsilon(path.polyline.get_point(i)));
    
    // get a copy; don't modify the orientation of the original loop object otherwise
    // next copies (if any) would not detect the correct orientation
    bool save_flipped = this->visitor_flipped;
    //ignore flip if can't reverse. we're a loop anyway.
    if (this->visitor_flipped && loop_to_seam.can_reverse()) {
        loop_to_seam.reverse();
    }
    this->visitor_flipped = false;

    // extrude all loops ccw
    //no! this was decided in perimeter_generator
    //but we need to know where is "inside", so we will use is_hole_loop. if is_hole_loop, then we need toconsider that the right direction is clockwise, else counter clockwise. 
    bool is_hole_loop = (loop_to_seam.loop_role() & ExtrusionLoopRole::elrHole) != 0;

    //if spiral vase, we have to ensure that all loops are in the same orientation.
    if (this->m_config.spiral_vase) {
        // loop_to_seam.make_counter_clockwise();
        if(!loop_to_seam.is_counter_clockwise())
            loop_to_seam.reverse();
        is_hole_loop = false;
    }
    for (const ExtrusionPath &path : loop_to_seam.paths)
        for (int i = 1; i < path.polyline.size(); ++i)
            assert(!path.polyline.get_point(i - 1).coincides_with_epsilon(path.polyline.get_point(i)));

    split_at_seam_pos(loop_to_seam, is_hole_loop);
    const coordf_t full_loop_length = loop_to_seam.length();
    const bool is_full_loop_ccw = loop_to_seam.polygon().is_counter_clockwise();
    //after that point, loop_to_seam can be modified by 'paths', so don't use it anymore
#if _DEBUG
    for (auto it = std::next(loop_to_seam.paths.begin()); it != loop_to_seam.paths.end(); ++it) {
        assert(it->polyline.size() >= 2);
        assert(std::prev(it)->polyline.back() == it->polyline.front());
    }
    assert(loop_to_seam.paths.front().first_point() == loop_to_seam.paths.back().last_point());
#endif
    // clip the path to avoid the extruder to get exactly on the first point of the loop;
    // if polyline was shorter than the clipping distance we'd get a null polyline, so
    // we discard it in that case
    ExtrusionPaths& building_paths = loop_to_seam.paths;
    for (const ExtrusionPath &path : building_paths)
        for (int i = 1; i < path.polyline.size(); ++i)
            assert(!path.polyline.get_point(i - 1).coincides_with_epsilon(path.polyline.get_point(i)));
    //direction is now set, make the path unreversable
    for (ExtrusionPath& path : building_paths) {
        //assert(!path.can_reverse() || !is_perimeter(path.role())); //just ensure the perimeter have their direction enforced.
        path.set_can_reverse(false);
    }
    if (m_enable_loop_clipping && m_writer.tool_is_extruder()) {
        coordf_t clip_length = scale_(m_config.seam_gap.get_abs_value(m_writer.tool()->id(), nozzle_diam));
        if (loop_to_seam.role() == ExtrusionRole::ExternalPerimeter) {
            coordf_t clip_length_external = scale_(m_config.seam_gap_external.get_abs_value(m_writer.tool()->id(), unscaled(clip_length)));
            if (clip_length_external > 0) {
                clip_length = clip_length_external;
            }
        }
        coordf_t min_clip_length = scale_(nozzle_diam) * 0.15;

        if (clip_length > full_loop_length / 4) {
            //fall back to min_clip_length
            if (clip_length > full_loop_length / 2) {
                clip_length = min_clip_length;
            } else {
                float percent = (float(clip_length) / (float(full_loop_length) / 4)) - 1;
                clip_length = clip_length * (1- percent) + min_clip_length * percent;
            }
        }
        if (clip_length < full_loop_length / 4) {
            // get paths
            ExtrusionPaths clipped;
            if (clip_length > min_clip_length) {
                    // remove clip_length, like normally, but keep the removed part
                clipped = clip_end(building_paths, clip_length);
                    // remove min_clip_length from the removed paths
                clip_end(clipped, min_clip_length);
                    // ensure that the removed paths are travels
                for (ExtrusionPath& ep : clipped)
                    ep.attributes_mutable().mm3_per_mm = 0;
                    // re-add removed paths as travels.
                append(building_paths, clipped);
            } else {
                clip_end(building_paths, clip_length);
            }
        }
    }
    if (building_paths.empty()) return "";

    const ExtrusionPaths& wipe_paths = building_paths;
    for (const ExtrusionPath &path : wipe_paths)
        for (int i = 1; i < path.polyline.size(); ++i)
            assert(!path.polyline.get_point(i - 1).coincides_with_epsilon(path.polyline.get_point(i)));

    ExtrusionPaths notch_extrusion_start;
    ExtrusionPaths notch_extrusion_end;
    // seam notch if applicable
    seam_notch(original_loop, building_paths, notch_extrusion_start, notch_extrusion_end, is_hole_loop, is_full_loop_ccw);

    const ExtrusionPaths& paths = building_paths;
    for (const ExtrusionPath &path : paths)
        for (int i = 1; i < path.polyline.size(); ++i)
            assert(!path.polyline.get_point(i - 1).coincides_with_epsilon(path.polyline.get_point(i)));

    // apply the small perimeter speed
    if (speed == -1 && (paths.front().role().is_perimeter()) && paths.front().role() != ExtrusionRole::ThinWall) {
        double min_length = scale_d(this->m_config.small_perimeter_min_length.get_abs_value(nozzle_diam));
        double max_length = scale_d(this->m_config.small_perimeter_max_length.get_abs_value(nozzle_diam));
        if (full_loop_length < max_length) {
            if (full_loop_length <= min_length) {
                speed = SMALL_PERIMETER_SPEED_RATIO_OFFSET;
            } else if (max_length > min_length) {
                //use a negative speed: it will be use as a ratio when computing the real speed
                speed = SMALL_PERIMETER_SPEED_RATIO_OFFSET - (full_loop_length - min_length) / (max_length - min_length);
            }
        }
    }

    std::string gcode;

    coordf_t point_dist_for_vec = scale_t(m_config.seam_gap.get_abs_value(m_writer.tool()->id(), nozzle_diam)) / 2;

    // generate the unretracting/wipe start move (same thing than for the end, but on the other side)
    assert(!wipe_paths.empty() && wipe_paths.front().size() > 1 && !wipe_paths.back().empty());
    if (EXTRUDER_CONFIG_WITH_DEFAULT(wipe_inside_start, true) && !wipe_paths.empty() && wipe_paths.front().size() > 1 && wipe_paths.back().size() > 1 && wipe_paths.front().role() == ExtrusionRole::ExternalPerimeter) {
        //note: previous & next are inverted to extrude "in the opposite direction, as we are "rewinding"
        //Point previous_point = wipe_paths.back().polyline.points.back();
        Point previous_point = wipe_paths.front().polyline.get_point_from_begin(std::min(wipe_paths.front().polyline.length() / 2, point_dist_for_vec));
        Point current_point = wipe_paths.front().first_point();
        //Point next_point = wipe_paths.front().polyline.get_points()[1];
        Point next_point = wipe_paths.front().last_point();
        if (next_point == current_point) {
            //can happen if seam_gap is null
            next_point = wipe_paths.back().polyline.get_point_from_end(std::min(wipe_paths.back().polyline.length() / 2, point_dist_for_vec));
        }
        if (next_point == current_point || previous_point == current_point) {
            throw Slic3r::SlicingError(_u8L("ExtrusionRole::ror while writing gcode: two points are at the same position. Please send the .3mf project to the dev team for debugging. Extrude loop: wipe_inside_start."));
        }
        Point a = next_point;  // second point
        Point b = previous_point;  // second to last point
        if (is_hole_loop ? (!is_full_loop_ccw) : (is_full_loop_ccw)) {
            // swap points
            Point c = a; a = b; b = c;
        }
        assert(ccw_angle_old_test(current_point, a, b) == abs_angle(angle_ccw( a-current_point,b-current_point)));
        double angle = abs_angle(angle_ccw( a-current_point,b-current_point)) / 3;

        // turn left if contour, turn right if hole
        if (is_hole_loop ? (!is_full_loop_ccw) : (is_full_loop_ccw)) angle *= -1;

        // create the destination point along the first segment and rotate it
        // we make sure we don't exceed the segment length because we don't know
        // the rotation of the second segment so we might cross the object boundary
        Vec2d  current_pos = current_point.cast<double>();
        Vec2d  next_pos = next_point.cast<double>();
        Vec2d  vec_dist = next_pos - current_pos;
        double vec_norm = vec_dist.norm();
        const double setting_max_depth = (m_config.wipe_inside_depth.get_abs_value(m_writer.tool()->id(), nozzle_diam));
        coordf_t dist = scale_d(nozzle_diam) / 2;
        Point  pt = (current_pos + vec_dist * (2 * dist / vec_norm)).cast<coord_t>();
        pt.rotate(angle, current_point);
        //check if we can go to higher dist
        if (nozzle_diam != 0 && setting_max_depth > nozzle_diam * 0.55) {
            // call travel_to to trigger retract, so we can check it (but don't use the travel)
            travel_to(gcode, pt, wipe_paths.front().role());
            if (m_writer.tool()->need_unretract()) {
                this->m_throw_if_canceled();
                dist = coordf_t(check_wipe::max_depth(wipe_paths, scale_t(setting_max_depth), scale_t(nozzle_diam), [current_pos, current_point, vec_dist, vec_norm, angle](coord_t dist)->Point {
                    Point pt = (current_pos + vec_dist * (2 * dist / vec_norm)).cast<coord_t>();
                    pt.rotate(angle, current_point);
                    return pt;
                    }));
            }
        }
        // Shift by no more than a nozzle diameter.
        //FIXME Hiding the seams will not work nicely for very densely discretized contours!
        pt = (/*(nd >= vec_norm) ? next_pos : */(current_pos + vec_dist * ( 2 * dist / vec_norm))).cast<coord_t>();
        pt.rotate(angle, current_point);
        //gcode += m_writer.travel_to_xy(this->point_to_gcode(pt), 0.0, "move inwards before retraction/seam");
        //this->set_last_pos(pt);
        // use extrude instead of travel_to_xy to trigger the unretract
        ExtrusionPath fake_path_wipe(ArcPolyline(Polyline{ pt , current_point }), wipe_paths.front().attributes(), wipe_paths.front().can_reverse());
        fake_path_wipe.attributes_mutable().mm3_per_mm = 0;
        assert(!fake_path_wipe.can_reverse());
        gcode += extrude_path(fake_path_wipe, "move inwards before retraction/seam", speed);
    }
    
    //extrusion notch start if any
    for (const ExtrusionPath& path : notch_extrusion_start) {
        assert(!path.can_reverse());
        gcode += extrude_path(path, description, speed);
    }
    // extrude along the path
    //FIXME: we can have one-point paths in the loop that don't move : it's useless! and can create problems!
    for (const ExtrusionPath& path : paths) {
        assert(!path.can_reverse());
        if(path.polyline.size() > 1)
            gcode += extrude_path(path, description, speed);
    }
    //extrusion notch end if any
    for (const ExtrusionPath& path : notch_extrusion_end) {
        assert(!path.can_reverse());
        gcode += extrude_path(path, description, speed);
    }

    // reset acceleration
    m_writer.set_acceleration((uint16_t)floor(get_default_acceleration(m_config) + 0.5));

    //basic wipe, may be erased after if we need a more complex one
    add_wipe_points(wipe_paths);

    //wipe for External Perimeter (and not vase)
    if (wipe_paths.back().role() == ExtrusionRole::ExternalPerimeter && m_layer != NULL && m_config.perimeters.value > 0 && wipe_paths.front().size() >= 2 && wipe_paths.back().polyline.size() >= 2
        && (m_enable_loop_clipping && m_writer.tool_is_extruder()) ) {
        double dist_wipe_extra_perimeter = EXTRUDER_CONFIG_WITH_DEFAULT(wipe_extra_perimeter, 0);

        //safeguard : if not possible to wipe, abord.
        if (wipe_paths.size() == 1 && wipe_paths.front().size() <= 2) {
            return gcode;
        }
        //TODO: abord if the wipe is too big for a mini loop (in a better way)
        if (wipe_paths.size() == 1 && unscaled(wipe_paths.front().length()) < EXTRUDER_CONFIG_WITH_DEFAULT(wipe_extra_perimeter, 0) + nozzle_diam) {
            return gcode;
        }
        //get dist for wipe point
        coordf_t dist_point = wipe_paths.back().width();
        //get points for wipe
        Point prev_point = wipe_paths.back().polyline.get_point_from_end(std::min(wipe_paths.back().polyline.length()/2, point_dist_for_vec));       // second to last point
        // *(wipe_paths.back().polyline.points.end() - 2) this is the same as (or should be) as wipe_paths.front().first_point();
        Point current_point = wipe_paths.front().first_point();
        Point next_point = wipe_paths.front().polyline.get_point_from_begin(std::min(wipe_paths.front().polyline.length()/2, point_dist_for_vec));  // second point
        //safeguard : if a ExtrusionRole::ror exist abord;
        if (next_point == current_point || prev_point == current_point) {
            throw Slic3r::SlicingError(_u8L("ExtrusionRole::ror while writing gcode: two points are at the same position. Please send the .3mf project to the dev team for debugging. Extrude loop: wipe."));
        }

        // start the wipe. Note: you have to end it! (no return before emmitting it)
        std::string start_wipe = ";" + GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Wipe_Start) + "\n";
        //extra wipe before the little move.
        if (dist_wipe_extra_perimeter > 0) {
            coordf_t wipe_dist = scale_(dist_wipe_extra_perimeter);
            ExtrusionPaths paths_wipe;
            m_wipe.reset_path();
            ArcPolyline wipe_polyline;
            for (int i = 0; i < wipe_paths.size(); i++) {
                const ExtrusionPath& path = wipe_paths[i];
                if (wipe_dist > 0) {
                    //first, we use the polyline for wipe_extra_perimeter
                    if (path.length() < wipe_dist) {
                        wipe_dist -= path.length();
                        paths_wipe.push_back(path);
                    } else {
                        paths_wipe.push_back(path);
                        paths_wipe.back().clip_end(path.length() - wipe_dist);

                        ExtrusionPath next_point_path = path;
                        next_point_path.reverse();
                        next_point_path.clip_end(wipe_dist);
                        next_point_path.reverse();
                        if (next_point_path.size() > 1) {
                            next_point = next_point_path.polyline.get_point_from_begin(std::min(next_point_path.length()/2, point_dist_for_vec));
                        } else if (i + 1 < wipe_paths.size()) {
                            next_point = wipe_paths[i + 1].first_point();
                        } else {
                            next_point = wipe_paths[0].first_point();
                        }
                        wipe_polyline.append(path.polyline);
                        wipe_polyline.clip_start(wipe_dist);
                        wipe_dist -= path.length();
                    }
                } else {
                    //then, it's stored for the wipe on retract
                    wipe_polyline.append(path.polyline);
                }
            }
            m_wipe.set_path(wipe_polyline.get_arc());
            //move
            for (ExtrusionPath& path : paths_wipe) {
                Point center;
                for (const Geometry::ArcWelder::Segment& segment : path.polyline.get_arc()) {
                    if (!start_wipe.empty()) {
                        gcode += start_wipe;
                        start_wipe = "";
                    }
                    coordf_t radius = segment.radius;
                    if (radius > 0) {
                        center = Geometry::ArcWelder::arc_center_scalar<coord_t, coordf_t>(current_point, segment.point, segment.radius, segment.ccw());
                        // Don't extrude a degenerated circle.
                        if (center.coincides_with_epsilon(current_point))
                            radius = 0;
                    }
                    if (radius == 0) {
                        gcode += m_writer.travel_to_xy(this->point_to_gcode(segment.point), 0.0, "; extra wipe"sv);
                    } else {
                        const Vec2d center_offset = this->point_to_gcode(center) - this->point_to_gcode(current_point);
                        coordf_t    angle         = Geometry::ArcWelder::arc_angle(current_point, segment.point, coord_t(radius));
                        assert(angle > 0);
                        const coordf_t line_length = angle * std::abs(radius);
                        gcode += m_writer.travel_arc_to_xy(this->point_to_gcode(segment.point), center_offset, segment.ccw(), 0.0, "; extra wipe"sv);
                    }
                    prev_point = current_point;
                    current_point = segment.point;
                    this->set_last_pos(current_point);
                }
            }
        }

        // make a little move inwards before leaving loop
        
        // detect angle between last and first segment
        // the side depends on the original winding order of the polygon (left for contours, right for holes)
        //FIXME improve the algorithm in case the loop is tiny.
        //FIXME improve the algorithm in case the loop is split into segments with a low number of points (see the Point b query).
        Point a = next_point;  // second point
        Point b = prev_point;  // second to last point
        if (is_hole_loop ? is_full_loop_ccw : (!is_full_loop_ccw)) {
            // swap points
            Point c = a; a = b; b = c;
        }
#ifdef _DEBUG
        double a1 = angle_ccw( a-current_point,b-current_point);
        double abs1 = abs_angle(angle_ccw( a-current_point,b-current_point));
        double a2 = ccw_angle_old_test(current_point, a, b);
        assert(is_approx(abs_angle(angle_ccw( a-current_point,b-current_point)), ccw_angle_old_test(current_point, a, b), 0.000000001));
#endif
        double angle = abs_angle(angle_ccw( a-current_point,b-current_point)) / 3;
        
        // turn left if contour, turn right if hole
        if (is_hole_loop ? is_full_loop_ccw : (!is_full_loop_ccw)) angle *= -1;

        // create the destination point along the first segment and rotate it
        // we make sure we don't exceed the segment length because we don't know
        // the rotation of the second segment so we might cross the object boundary
        Vec2d  current_pos = current_point.cast<double>();
        Vec2d  next_pos = next_point.cast<double>();
        Vec2d  vec_dist = next_pos - current_pos;
        double vec_norm = vec_dist.norm();
        double sin_a    = std::abs(std::sin(angle));
        sin_a = std::max(0.1, sin_a);
        const double setting_max_depth = (m_config.wipe_inside_depth.get_abs_value(m_writer.tool()->id(), nozzle_diam));
        coordf_t dist   = setting_max_depth <= 0 ? scale_d(nozzle_diam) / 2 : scale_d(setting_max_depth);
        if (nozzle_diam != 0 && setting_max_depth > nozzle_diam * 0.55)
            dist = coordf_t(check_wipe::max_depth(wipe_paths, scale_t(setting_max_depth), scale_t(nozzle_diam), 
                [current_pos, current_point, vec_dist, vec_norm, angle, sin_a](coord_t dist)->Point {
                    Point pt = (current_pos + vec_dist * (dist / (vec_norm * sin_a))).cast<coord_t>();
                    pt.rotate(angle, current_point);
                    return pt;
                }));
        // Shift by no more than a nozzle diameter.
        // FIXME Hiding the seams will not work nicely for very densely discretized contours!
        Point pt_inside = (/*(nd >= vec_norm) ? next_pos : */ (current_pos + vec_dist * ( dist / (vec_norm * sin_a))))
                              .cast<coord_t>();
        pt_inside.rotate(angle, current_point);

        if (EXTRUDER_CONFIG_WITH_DEFAULT(wipe_inside_end, true)) {
            if (!m_wipe.is_enabled()) {
                if (!start_wipe.empty()) {
                    gcode += start_wipe;
                    start_wipe = "";
                }
                // generate the travel move
                gcode += m_writer.travel_to_xy(this->point_to_gcode(pt_inside), 0.0, "move inwards before travel");
                this->set_last_pos(pt_inside);
            } else {
                // also shift the wipe on retract if wipe_inside_end
                // go to the inside (use clipper for easy shift)
                Polygon original_polygon = original_loop.polygon();
                for (int i = 1; i < original_polygon.points.size(); ++i)
                    assert(!original_polygon.points[i - 1].coincides_with_epsilon(original_polygon.points[i]));
                Polygons polys = offset(original_polygon, -dist);
                if (!polys.empty()) {
                    // if multiple polygon, keep only our nearest.
                    if (polys.size() > 1) {
                        Point nearest_pt;
                        size_t nearest_pt_idx;
                        size_t   nearest_poly_idx = size_t(-1);
                        coordf_t best_dist_sqr    = dist * dist * 100;
                        for (int idx_poly = 0; idx_poly < polys.size(); ++idx_poly) {
                            Polygon &poly = polys[idx_poly];
                            // use projection  
                            auto [near_pt, near_idx] = poly.point_projection(pt_inside);
                            if (coordf_t test_dist = pt_inside.distance_to_square(near_pt);
                                test_dist < best_dist_sqr) {
                                nearest_poly_idx = idx_poly;
                                best_dist_sqr    = test_dist;
                                nearest_pt       = near_pt;
                                nearest_pt_idx   = near_idx;
                            }
                        }
                        if (nearest_poly_idx == size_t(-1)) {
                            // too far away, try with lower offset
                            polys = offset(original_polygon, -dist / 2);
                            assert(!polys.empty());
                            if (!polys.empty()) {
                                for (int idx_poly = 0; idx_poly < polys.size(); ++idx_poly) {
                                    Polygon &poly = polys[idx_poly];
                                    auto [near_pt, near_idx] = poly.point_projection(pt_inside);
                                    if (coordf_t test_dist = pt_inside.distance_to_square(near_pt);
                                        test_dist < best_dist_sqr) {
                                        nearest_poly_idx = idx_poly;
                                        best_dist_sqr    = test_dist;
                                        nearest_pt       = near_pt;
                                        nearest_pt_idx   = near_idx;
                                    }
                                }
                            }
                            // if fail again (weird) reuse our initial poly
                        }
                        assert(nearest_poly_idx < polys.size());
                        if (nearest_poly_idx < polys.size()) {
                            if (nearest_poly_idx < polys.size() - 1)
                                polys.erase(polys.begin() + nearest_poly_idx + 1, polys.end());
                            if (nearest_poly_idx > 0 )
                                polys.erase(polys.begin(), polys.begin() + nearest_poly_idx);
                            assert(polys.size() == 1);
                            assert(nearest_pt_idx < polys.front().points.size());
                            if (nearest_pt_idx < polys.front().points.size() &&
                                !polys.front().points[nearest_pt_idx].coincides_with_epsilon(nearest_pt)) {
                                polys.front().points.insert(polys.front().points.begin() + nearest_pt_idx, nearest_pt);
                            }
                            assert(polys.front().closest_point(pt_inside) != nullptr &&
                                   std::abs(polys.front().closest_point(pt_inside)->distance_to_square(pt_inside) -
                                            best_dist_sqr) < SCALED_EPSILON);
                        } else {
                            polys = { original_polygon };
                        }
                    }

                    // This offset may put point so close that they coincides.
                    // So we need to remove points that are now too close.
                    assert(!paths.empty());
                    coordf_t min_dist_sqr = scale_d(paths.front().width()) / 10;
                    min_dist_sqr *= min_dist_sqr;
                    int      pop_back = 0;
                    Point    last_pop_back;
                    int      pop_in = 0;
                    Point    last_pop_in;
                    Polygon &poly = polys.front();
                    for (int idxpt = 1; idxpt < poly.points.size(); ++idxpt) {
                        if (poly.points[idxpt - 1].distance_to_square(poly.points[idxpt]) < min_dist_sqr) {
                            last_pop_in = poly.points[idxpt];
                            poly.points.erase(poly.points.begin() + idxpt);
                            idxpt--;
                            pop_in++;
                        }
                    }
                    if (poly.size() > 1 && poly.front().distance_to_square(poly.back()) < min_dist_sqr) {
                        last_pop_back = poly.points.back();
                        poly.points.pop_back();
                        pop_back++;
                    }
                    if (poly.size() < 3) {
                        polys.clear();
                    }
                }
                // find nearest point
                size_t         best_poly_idx = 0;
                size_t         best_pt_idx   = 0;
                const coordf_t max_sqr_dist  = dist * dist * 8; // 2*nozzle²
                coordf_t       best_sqr_dist = max_sqr_dist;
                Point          start_point   = pt_inside;
                if (!polys.empty()) {
                    Polygon &poly = polys.front();
                    for (int i = 1; i < poly.points.size(); ++i)
                        assert(!poly.points[i - 1].coincides_with_epsilon(poly.points[i]));
                    if (poly.is_clockwise() ^ original_polygon.is_clockwise())
                        poly.reverse();
                    for (size_t pt_idx = 0; pt_idx < poly.size(); pt_idx++) {
                        if (poly.points[pt_idx].distance_to_square(pt_inside) < best_sqr_dist) {
                            best_sqr_dist = poly.points[pt_idx].distance_to_square(pt_inside);
                            best_pt_idx   = pt_idx;
                        }
                    }
                    if (best_sqr_dist == max_sqr_dist) {
                        // fail to find nearest point, try to find an edge
                        if (poly.is_clockwise() ^ original_polygon.is_clockwise())
                            poly.reverse();
                        poly.points.push_back(poly.points.front());
                        for (size_t pt_idx = 0; pt_idx < poly.points.size() - 1; pt_idx++) {
                            if (Line{poly.points[pt_idx], poly.points[pt_idx + 1]}.distance_to_squared(pt_inside) <
                                best_sqr_dist) {
                                poly.points.insert(poly.points.begin() + pt_idx + 1, pt_inside);
                                best_sqr_dist = 0;
                                best_pt_idx   = pt_idx + 1;
                                start_point   = pt_inside.projection_onto(
                                    poly.points[pt_idx], poly.points[pt_idx + 1]);
                                poly.points.erase(poly.points.end() - 1);
                                break;
                            }
                        }
                    } else {
                        // check if the point is "before" or "after"
                        // get the intersection with line that start with the best point (works if the point is before
                        // us, ie in the wrong dir)
                        Point pt_if_before;
                        if (best_pt_idx + 1 < poly.size()) {
                            pt_if_before = pt_inside.projection_onto(poly.points[best_pt_idx], poly.points[best_pt_idx + 1]);
                        } else {
                            pt_if_before = pt_inside.projection_onto(poly.points[best_pt_idx], poly.points.front());
                        }
                        // get the intersection with line that end with the best point (works if the point is after
                        // us, ie in the good dir)
                        Point pt_if_after;
                        if (best_pt_idx > 0) {
                            pt_if_after = pt_inside.projection_onto(poly.points[best_pt_idx - 1], poly.points[best_pt_idx]);
                        } else {
                            pt_if_after = pt_inside.projection_onto(poly.points.back(), poly.points[best_pt_idx]);
                        }
                        // choose
                        if (pt_if_before.distance_to_square(pt_inside) > pt_if_after.distance_to_square(pt_inside)) {
                            start_point = pt_if_after;
                        } else {
                            start_point = pt_if_before;
                            best_pt_idx = (best_pt_idx + 1) % poly.size();
                        }
                    }
                }
                if (best_sqr_dist == max_sqr_dist || polys.empty()) {
                    // can't find a path, use the old one
                    //BOOST_LOG_TRIVIAL(warning) << "Warn: can't find a proper path for wipe on retract. Layer " << m_layer_index << ", pos " << this->point_to_gcode(pt).x() << " : " << this->point_to_gcode(pt).y() << " !";
                } else {
                    Polygon &poly = polys.front();
                    m_wipe.reset_path();
                    ArcPolyline wipe_path;
                    // add first point if not redondant
                    if (poly.points[best_pt_idx].distance_to_square(start_point) > SCALED_EPSILON * SCALED_EPSILON * 100)
                        wipe_path.append(start_point);
                    // get the points from here
                    for (size_t pt_idx = best_pt_idx; pt_idx < poly.points.size(); pt_idx++) {
                        wipe_path.append(poly.points[pt_idx]);
                    }
                    for (size_t pt_idx = 0; pt_idx < best_pt_idx; pt_idx++) { wipe_path.append(poly.points[pt_idx]); }
                    m_wipe.set_path(std::move(wipe_path.get_arc()));
                }
                
                if (!start_wipe.empty()) {
                    gcode += start_wipe;
                    start_wipe = "";
                }
                // generate the travel move
                gcode += m_writer.travel_to_xy(this->point_to_gcode(start_point), 0.0, "move inwards before wipe");
                this->set_last_pos(start_point);
            }

        }
        // if we started wiping, then end the wipe section.
        if (start_wipe.empty()) {
            gcode += ";" + GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Wipe_End) + "\n";
        }
    }

    assert(!this->visitor_flipped);
    this->visitor_flipped = save_flipped;
    return gcode;
}

template <typename THING>
void GCodeGenerator::add_wipe_points(const std::vector<THING>& paths, bool reverse /*= true*/) {
    if (m_wipe.is_enabled()) {
        ArcPolyline wipe_polyline;
        for (const THING& path : paths) {
            if (path.role().is_bridge())
                break; // Do not perform a wipe on bridges.
            assert(path.polyline.size() >= 2);
            assert(wipe_polyline.empty() || wipe_polyline.back().coincides_with_epsilon(path.first_point()));
            if (!wipe_polyline.empty() && !wipe_polyline.back().coincides_with_epsilon(path.first_point()))
                break; // ExtrusionMultiPath is interrupted in some place.

            wipe_polyline.append(path.polyline);
        }
        if(reverse)
            wipe_polyline.reverse();
        m_wipe.set_path(wipe_polyline.get_arc());
    }
}

std::string GCodeGenerator::extrude_multi_path(const ExtrusionMultiPath &multipath, const std::string_view description, double speed) {
#ifndef NDEBUG
    assert(!multipath.empty());
    assert(!multipath.paths.front().polyline.empty());
    for (auto it = std::next(multipath.paths.begin()); it != multipath.paths.end(); ++it) {
        assert(it->polyline.size() >= 2);
        assert(std::prev(it)->polyline.back() == it->polyline.front());
    }
#endif // NDEBUG
    std::string gcode;
    //test if we reverse
    bool should_reverse = this->visitor_flipped;
    if(should_reverse) //TODO: rethink that
        should_reverse = !(last_pos_defined() && multipath.can_reverse() 
            && multipath.last_point().distance_to_square(last_pos()) > multipath.first_point().distance_to_square(last_pos()));
    else
        should_reverse = last_pos_defined() && multipath.can_reverse() 
            && multipath.first_point().distance_to_square(last_pos()) > multipath.last_point().distance_to_square(last_pos());

    bool saved_flipped = this->visitor_flipped;
    if (should_reverse) {
        this->visitor_flipped = true;
        //reverse to get a shorter point (hopefully there is still no feature that choose a point that need no perimeter crossing before).
        // extrude along the  reversedpath
        for (size_t idx_path = multipath.paths.size() - 1; idx_path < multipath.paths.size(); --idx_path) {
            //it's possible to have un-reverseable paths into a reversable multipath: this means that only the whole thing can be reversed, and not individual apths.
            if (multipath.paths[idx_path].can_reverse()) {
                // extrude_path will reverse the path by itself, no need to copy it do to it here.
                gcode += extrude_path(multipath.paths[idx_path], description, speed);
            } else {
                ExtrusionPath path = multipath.paths[idx_path];
                path.reverse();
                gcode += extrude_path(path, description, speed);
            }
        }
        add_wipe_points(multipath.paths, false);
    } else {
        this->visitor_flipped = false;
        // extrude along the path
        for (const ExtrusionPath& path : multipath.paths) {
            gcode += extrude_path(path, description, speed);
        }
        add_wipe_points(multipath.paths, true);
    };
    this->visitor_flipped = saved_flipped;
    // reset acceleration
    m_writer.set_acceleration((uint16_t)floor(get_default_acceleration(m_config) + 0.5));
    return gcode;
}

std::string GCodeGenerator::extrude_multi_path3D(const ExtrusionMultiPath3D &multipath3D, const std::string_view description, double speed) {
    
    //test if we reverse
    bool should_reverse = this->visitor_flipped;
    if(should_reverse)
        should_reverse = !(last_pos_defined() && multipath3D.can_reverse() 
            && multipath3D.last_point().distance_to_square(last_pos()) > multipath3D.first_point().distance_to_square(last_pos()));
    else
        should_reverse = last_pos_defined() && multipath3D.can_reverse() 
            && multipath3D.first_point().distance_to_square(last_pos()) > multipath3D.last_point().distance_to_square(last_pos());
    
    std::string gcode;
    auto extrudepath3D =
        [&](const ExtrusionPath3D &path) {
            gcode += this->_before_extrude(path, description, speed);

            // calculate extrusion length per distance unit
            double e_per_mm = _compute_e_per_mm(path.mm3_per_mm());
            double path_length = 0.;
            {
                std::string_view comment = m_writer.gcode_config().gcode_comments ? description : ""sv;
                // for (const Line &line : path.polyline.lines()) {
                for (size_t i = 0; i < path.polyline.size() - 1; i++) {
                    assert(!path.as_polyline().has_arc()); // FIXME extrude_arc_to_xyz
                    Line         line(path.polyline.get_point(i), path.polyline.get_point(i + 1));
                    const double line_length = line.length() * SCALING_FACTOR;
                    path_length += line_length;
                    gcode += m_writer.extrude_to_xyz(this->point_to_gcode(line.b, path.z_offsets.size() > i + 1 ? path.z_offsets[i + 1] : 0),
                                                     e_per_mm * line_length, comment);
                }
            }
            gcode += this->_after_extrude(path);
        };
    // extrude along the path
    bool saved_flipped = this->visitor_flipped;
    if (should_reverse) {
        this->visitor_flipped = true;
        // reverse to get a shorter point (hopefully there is still no feature that choose a point that need no perimeter crossing before).
        // extrude along the  reversedpath
        for (size_t idx_path = multipath3D.paths.size() - 1; idx_path < multipath3D.paths.size(); --idx_path) {
            assert(multipath3D.paths[idx_path].can_reverse());
            // extrude_path will reverse the path by itself, no need to copy it do to it here.
            gcode += extrude_path(multipath3D.paths[idx_path], description, speed);
        }
        add_wipe_points(multipath3D.paths, false);
    } else {
        this->visitor_flipped = false;
        for (const ExtrusionPath3D &path : multipath3D.paths) {
            extrudepath3D(path);
        }
        add_wipe_points(multipath3D.paths, true);
    }
    this->visitor_flipped = saved_flipped;
    // reset acceleration
    m_writer.set_acceleration((uint16_t)floor(get_default_acceleration(m_config) + 0.5));
    return gcode;
}

std::string GCodeGenerator::extrude_entity(const ExtrusionEntityReference &entity, const std::string_view description, double speed)
{
    assert(!visitor_in_use);
    this->visitor_in_use = true;
    this->visitor_gcode.clear();
    this->visitor_comment = description;
    this->visitor_speed = speed;
    this->visitor_flipped = entity.flipped();
    entity.extrusion_entity().visit(*this);
    this->visitor_in_use = false;
    return this->visitor_gcode;
}

void GCodeGenerator::use(const ExtrusionEntityCollection &collection) {
    if (!collection.can_sort() /*|| collection.role() == ExtrusionRole::Mixed*/ || collection.entities().size() <= 1) {
        for (const ExtrusionEntity* next_entity : collection.entities()) {
            next_entity->visit(*this);
        }
    } else {
        bool reversed = this->visitor_flipped;
        ExtrusionEntityReferences chained = chain_extrusion_references(collection, &last_pos());
        for (const ExtrusionEntityReference &next_entity : chained) {
            this->visitor_flipped = reversed != next_entity.flipped();
            next_entity.extrusion_entity().visit(*this);
        }
        this->visitor_flipped = reversed;
    }
}

std::string GCodeGenerator::extrude_path(const ExtrusionPath &path, const std::string_view description, double speed_mm_per_sec) {
    std::string gcode;
    ExtrusionPath simplifed_path = path;
    for (int i = 1; i < simplifed_path.polyline.size(); ++i)
        assert(!simplifed_path.polyline.get_point(i - 1).coincides_with_epsilon(simplifed_path.polyline.get_point(i)));
    if (this->visitor_flipped) {
        assert(path.can_reverse());
        simplifed_path.reverse();
    }

    //check if we should reverse it
    if (last_pos_defined() && path.can_reverse()
        && simplifed_path.first_point().distance_to_square(last_pos()) > simplifed_path.last_point().distance_to_square(last_pos())) {
        simplifed_path.reverse();
    }

    // simplify with gcode_resolution (not used yet). Simplify by jusntion deviation before the g1/sec count, to be able to use that decimation to reduce max_gcode_per_second triggers.
    // But as it can be visible on cylinders, should only be called if a max_gcode_per_second trigger may come.
    //simplifed_path.simplify(m_scaled_gcode_resolution);
    const coordf_t scaled_min_length = scale_d(this->config().min_length.value);
    const double max_gcode_per_second = this->config().max_gcode_per_second.value;
    coordf_t current_scaled_min_length = scaled_min_length;
    double fan_speed;
    if (max_gcode_per_second > 0) {
        current_scaled_min_length = std::max(current_scaled_min_length, scale_d(_compute_speed_mm_per_sec(path, speed_mm_per_sec, fan_speed, nullptr)) / max_gcode_per_second);
    }
    if (current_scaled_min_length > 0 && !m_last_too_small.empty()) {
        //ensure that it's a continous thing of the same type
        if (m_last_too_small.last_point().distance_to_square(path.first_point()) < EPSILON * EPSILON * 4 && 
            (path.role() == m_last_too_small.role() || m_last_too_small.length() < scale_d(m_last_too_small.width()/10))) {
            simplifed_path.attributes_mutable().height = float(m_last_too_small.height() * m_last_too_small.length() + simplifed_path.height() * simplifed_path.length()) / float(m_last_too_small.length() + simplifed_path.length());
            simplifed_path.attributes_mutable().mm3_per_mm = (m_last_too_small.mm3_per_mm() * m_last_too_small.length() + simplifed_path.mm3_per_mm() * simplifed_path.length()) / (m_last_too_small.length() + simplifed_path.length());
            m_last_too_small.polyline.append(simplifed_path.polyline);
            simplifed_path.polyline.swap(m_last_too_small.polyline);
            assert(simplifed_path.height() == simplifed_path.height());
            assert(simplifed_path.mm3_per_mm() == simplifed_path.mm3_per_mm());
            m_last_too_small.polyline.clear();
        } else {
            //finish extrude the little thing that was left before us and incompatible with our next extrusion.
            ExtrusionPath to_finish = m_last_too_small;
            gcode += this->_extrude(m_last_too_small, m_last_description, m_last_speed_mm_per_sec);
            m_last_too_small.polyline.clear();
        }
    }
    if (current_scaled_min_length > 0 && (config().arc_fitting.value == ArcFittingType::Disabled || !simplifed_path.polyline.has_arc() || config().spiral_vase)) {
        // it's an alternative to simplifed_path.simplify(scale_(this->config().min_length)); with more enphasis on the segment length that on the feature detail.
        // because tolerance = min_length /10, douglas_peucker will erase more points if angles are shallower than 6° and then the '_plus' will kick in to keep a bit more.
        // if angles are all bigger than 6°, then the douglas_peucker will do all the work.
        assert(!simplifed_path.polyline.has_arc());
        simplifed_path.polyline = ArcPolyline(Polyline(MultiPoint::douglas_peucker_plus(simplifed_path.polyline.to_polyline().points, current_scaled_min_length / 10, current_scaled_min_length)));
    }
    if (scaled_min_length > 0 && simplifed_path.length() < scaled_min_length) {
        m_last_too_small = simplifed_path;
        m_last_description = description;
        m_last_speed_mm_per_sec = speed_mm_per_sec;
        return gcode;
    }

    for(int i=1;i<simplifed_path.polyline.size();++i)
        assert(!simplifed_path.polyline.get_point(i - 1).coincides_with_epsilon(simplifed_path.polyline.get_point(i)));
    gcode += this->_extrude(simplifed_path, description, speed_mm_per_sec);

    //simplifed_path will be discarded i can reuse it to create the wipe
    if (m_wipe.is_enabled()) {
        simplifed_path.reverse();
        m_wipe.set_path(simplifed_path.polyline.get_arc());
    }
    // reset acceleration
    m_writer.set_acceleration((uint16_t)floor(get_default_acceleration(m_config) + 0.5));
    return gcode;
}

std::string GCodeGenerator::extrude_path_3D(const ExtrusionPath3D &path, const std::string_view description, double speed) {
    //path.simplify(SCALED_RESOLUTION);
    ExtrusionPath3D simplifed_path = path;
    if (this->visitor_flipped) {
        assert(path.can_reverse());
        simplifed_path.reverse();
    }

    //check if we should reverse it
    if (last_pos_defined() && path.can_reverse()
        && simplifed_path.first_point().distance_to_square(last_pos()) > simplifed_path.last_point().distance_to_square(last_pos())) {
        simplifed_path.reverse();
    }

    std::string gcode = this->_before_extrude(simplifed_path, description, speed);

    // calculate extrusion length per distance unit
    double e_per_mm = _compute_e_per_mm(simplifed_path.mm3_per_mm());
    double path_length = 0.;
    {
        std::string_view comment = m_config.gcode_comments ? description : ""sv;
        //for (const Line &line : simplifed_path.polyline.lines()) {
        for (size_t i = 0; i < simplifed_path.polyline.size()-1;i++) {
            assert(!simplifed_path.polyline.has_arc()); //FIXME extrude_to_arc_xyz ?
            Line line(simplifed_path.polyline.get_point(i), simplifed_path.polyline.get_point(i + 1));
            const double line_length = line.length() * SCALING_FACTOR;
            path_length += line_length;
            gcode += m_writer.extrude_to_xyz(
                this->point_to_gcode(line.b, simplifed_path.z_offsets.size()>i ? simplifed_path.z_offsets[i] : 0),
                e_per_mm * line_length,
                comment);
        }
    }
    gcode += this->_after_extrude(simplifed_path);

    if (m_wipe.is_enabled()) {
        ArcPolyline temp = simplifed_path.as_polyline();
        temp.reverse();
        m_wipe.set_path(std::move(temp.get_arc()));
    }
    // reset acceleration
    m_writer.set_acceleration((uint16_t)floor(get_default_acceleration(m_config) + 0.5));
    return gcode;
}

void GCodeGenerator::set_region_for_extrude(const Print &print, const PrintObject *print_object, std::string &gcode)
{
    const PrintRegionConfig &region_config = this->m_region == nullptr ? 
        //FIXME
        (print_object == nullptr ? print.default_region_config() : print_object->default_region_config(print.default_region_config()) ) :
        //print.default_region_config() :
        m_region->config();
    // modify our fullprintconfig with it. (works as all items avaialable in the regionconfig are present in this config, ie: it write everything region-defined)
    m_config.apply(region_config);
    // pass our region config to the gcode writer
    m_writer.apply_print_region_config(region_config);
    // perimeter-only (but won't break anything if done also in infill & ironing): pass needed settings to seam placer.
    m_seam_placer.external_perimeters_first = region_config.external_perimeters_first.value;
    // temperature override from region
    if (m_layer != nullptr && m_layer->bottom_z() < EPSILON && m_config.print_first_layer_temperature.value > 0) {
        gcode += m_writer.set_temperature(m_config.print_first_layer_temperature.value, false, m_writer.tool()->id());
    } else if (m_config.print_temperature > 0) {
        gcode += m_writer.set_temperature(m_config.print_temperature.value, false, m_writer.tool()->id());
    } else if (m_layer != nullptr && m_layer->bottom_z() < EPSILON && m_config.first_layer_temperature.get_at(m_writer.tool()->id()) > 0) {
        gcode += m_writer.set_temperature(m_config.first_layer_temperature.get_at(m_writer.tool()->id()), false, m_writer.tool()->id());
    } else if (m_config.temperature.get_at(m_writer.tool()->id()) > 0) { // don't set it if disabled
        gcode += m_writer.set_temperature(m_config.temperature.get_at(m_writer.tool()->id()), false, m_writer.tool()->id());
    }
    // apply region_gcode
    if (!region_config.region_gcode.value.empty()) {
//TODO 2.7: new placeholder_parser_process call
        DynamicConfig config;
        config.set_key_value("layer_num", new ConfigOptionInt(m_layer_index));
        config.set_key_value("layer_z", new ConfigOptionFloat(m_layer == nullptr ? m_last_height : m_layer->print_z));
        assert(!m_gcode_label_objects_in_session || !m_gcode_label_objects_start.empty());
        m_gcode_label_objects_start += this->placeholder_parser_process("region_gcode",
                                                                        region_config.region_gcode.value,
                                                                        m_writer.tool()->id(), &config) +
                                       "\n";
    }
}

// Extrude perimeters: Decide where to put seams (hide or align seams).
void GCodeGenerator::extrude_perimeters(const ExtrudeArgs &print_args, const LayerIsland &island, std::string &gcode)
{
    m_seam_perimeters = true;
    const LayerRegion &layerm = *layer()->get_region(island.perimeters.region());
    // PrintObjects own the PrintRegions, thus the pointer to PrintRegion would be unique to a PrintObject, they would not
    // identify the content of PrintRegion accross the whole print uniquely. Translate to a Print specific PrintRegion.
    const Print       &print  = *print_args.print_instance.print_object.print();
    m_region = &print.get_print_region(layerm.region().print_region_id());
    set_region_for_extrude(print, nullptr, gcode);
    bool first = true;
    std::vector<const ExtrusionEntity*> to_extrude;
    for (uint32_t perimeter_id : island.perimeters) {
        // Extrusions inside islands are expected to be ordered already.
        // Don't reorder them.
        assert(dynamic_cast<const ExtrusionEntityCollection*>(layerm.perimeters().entities()[perimeter_id]));
        if (const auto *eec = static_cast<const ExtrusionEntityCollection*>(layerm.perimeters().entities()[perimeter_id]);
            shall_print_this_extrusion_collection(print_args, eec, *m_region)) {
            // This may not apply to Arachne, but maybe the Arachne gap fill should disable reverse as well?
            // assert(! eec->can_reverse());
            if (first) {
                first = false;
                // Apply region-specific settings
            }
            to_extrude.push_back(eec);
        }
    }
    ExtrusionEntityReferences chained = chain_extrusion_references(to_extrude, &last_pos());
    for (const ExtrusionEntityReference &next_entity : chained) {
        gcode += this->extrude_entity(next_entity, comment_perimeter, -1.);
        if (m_travel_obstacle_tracker.is_init())
            m_travel_obstacle_tracker.mark_extruded(&next_entity.extrusion_entity(),
                                                    print_args.print_instance.object_layer_to_print_id,
                                                    print_args.print_instance.instance_id);
    }
    m_region = nullptr;
    m_seam_perimeters = false;
}

// Chain the paths hierarchically by a greedy algorithm to minimize a travel distance.
void GCodeGenerator::extrude_infill(const ExtrudeArgs& print_args, const LayerIsland &island, bool is_infill_first, std::string &gcode)
{
    ExtrusionEntityCollection temp_fill_extrusions;
    const Print &print  = *print_args.print_instance.print_object.print();
    for (auto it = island.fills.begin(); it != island.fills.end();) {
        // Gather range of fill ranges with the same region.
        auto it_end = it;
        for (++ it_end; it_end != island.fills.end() && it->region() == it_end->region(); ++ it_end) ;
        const LayerRegion &layerm = *layer()->get_region(it->region());
        const ExtrusionEntityCollection &fills = layerm.fills();
        LayerExtrusionRanges::const_iterator it_fill_ranges_begin = it;
        LayerExtrusionRanges::const_iterator it_fill_ranges_end = it_end;
        // PrintObjects own the PrintRegions, thus the pointer to PrintRegion would be unique to a PrintObject, they would not
                // identify the content of PrintRegion accross the whole print uniquely. Translate to a Print specific PrintRegion.
        m_region = &print.get_print_region(layerm.region().print_region_id());
        temp_fill_extrusions.clear();
        for (auto it_fill_range = it_fill_ranges_begin; it_fill_range != it_fill_ranges_end; ++ it_fill_range) {
            assert(it_fill_range->region() == it_fill_ranges_begin->region());
            for (uint32_t fill_id : *it_fill_range) {
                assert(dynamic_cast<ExtrusionEntityCollection*>(fills.entities()[fill_id]));
                if (auto *eec = static_cast<ExtrusionEntityCollection*>(fills.entities()[fill_id]); shall_print_this_extrusion_collection(print_args, eec, layerm.region())) {
                    eec->flatten(true, temp_fill_extrusions);
                }
            }
        }
        if (!temp_fill_extrusions.empty() && (m_region->config().infill_first == is_infill_first)) {
            set_region_for_extrude(print, nullptr, gcode);
            for (const ExtrusionEntityReference &fill : chain_extrusion_references(temp_fill_extrusions, &last_pos()))
                gcode += this->extrude_entity(fill, "infill"sv);
        }
        it = it_end;
        m_region = nullptr;
    }
}

// Chain the paths hierarchically by a greedy algorithm to minimize a travel distance.
void GCodeGenerator::extrude_ironing(const ExtrudeArgs &print_args, const LayerIsland &island, std::string &gcode)
{
    ExtrusionEntityCollection temp_fill_extrusions;
    const Print &print  = *print_args.print_instance.print_object.print();
    for (auto it = island.ironings.begin(); it != island.ironings.end();) {
        // Gather range of fill ranges with the same region.
        auto it_end = it;
        for (++ it_end; it_end != island.ironings.end() && it->region() == it_end->region(); ++ it_end) ;
        const LayerRegion &layerm = *layer()->get_region(it->region());
        const ExtrusionEntityCollection &ironings = layerm.ironings();
        LayerExtrusionRanges::const_iterator it_fill_ranges_begin = it;
        LayerExtrusionRanges::const_iterator it_fill_ranges_end = it_end;
        // PrintObjects own the PrintRegions, thus the pointer to PrintRegion would be unique to a PrintObject, they would not
                // identify the content of PrintRegion accross the whole print uniquely. Translate to a Print specific PrintRegion.
        m_region = &print.get_print_region(layerm.region().print_region_id());
        temp_fill_extrusions.clear();
        for (auto it_fill_range = it_fill_ranges_begin; it_fill_range != it_fill_ranges_end; ++ it_fill_range) {
            assert(it_fill_range->region() == it_fill_ranges_begin->region());
            for (uint32_t fill_id : *it_fill_range) {
                assert(dynamic_cast<ExtrusionEntityCollection*>(ironings.entities()[fill_id]));
                if (auto *eec = static_cast<ExtrusionEntityCollection*>(ironings.entities()[fill_id]); shall_print_this_extrusion_collection(print_args, eec, layerm.region())) {
                    eec->flatten(true, temp_fill_extrusions);
                }
            }
        }
        if (!temp_fill_extrusions.empty()) {
            set_region_for_extrude(print, nullptr, gcode);
            for (const ExtrusionEntityReference &fill : chain_extrusion_references(temp_fill_extrusions, &last_pos()))
                gcode += this->extrude_entity(fill, "ironing"sv);
        }
        it = it_end;
        m_region = nullptr;
    }
}

void GCodeGenerator::extrude_skirt(
    ExtrusionLoop &loop_src, const ExtrusionFlow &extrusion_flow_override, std::string &gcode, const std::string_view description)
{
    assert(loop_src.is_counter_clockwise());

    if (loop_src.paths.empty())
        return;

    for (ExtrusionPath &path : loop_src.paths) {
        // Override extrusion parameters.
        assert(!std::isnan(extrusion_flow_override.mm3_per_mm));
        assert(!std::isnan(extrusion_flow_override.height));
        path.attributes_mutable().mm3_per_mm = extrusion_flow_override.mm3_per_mm;
        path.attributes_mutable().height     = extrusion_flow_override.height;
        //gcode += this->extrude_loop(loop_src, description, -1);
        // use extrude_entity to init "visitor fields".
        gcode += this->extrude_entity({loop_src, false}, description, -1);
    }

    if (m_wipe.is_enabled())
        // Wipe will hide the seam.
        m_wipe.set_path(loop_src.paths, false);

}

std::string GCodeGenerator::extrude_support(const ExtrusionEntityReferences &support_fills)
{
    static constexpr const auto support_label            = "support material"sv;
    static constexpr const auto support_interface_label  = "support material interface"sv;

    std::string gcode;
    if (! support_fills.empty()) {
        const double  support_speed            = m_config.get_computed_value("support_material_speed");
        const double  support_interface_speed  = m_config.get_computed_value("support_material_interface_speed");
        for (const ExtrusionEntityReference &eref : support_fills) {
            ExtrusionRole role = eref.extrusion_entity().role();
            assert(role == ExtrusionRole::SupportMaterial || role == ExtrusionRole::SupportMaterialInterface || role == ExtrusionRole::Mixed);
            //can have multiple level of collection.
            //don't use visitor to avoid reordering?
            // TODO check visitor with flip
            std::string_view label = (role == ExtrusionRole::SupportMaterial) ? support_label : support_interface_label;
            const double     speed = (role == ExtrusionRole::SupportMaterial) ? support_speed : support_interface_speed;
            assert(!visitor_in_use);
            this->visitor_in_use = true;
            visitor_gcode          = "";
            visitor_comment        = label;
            visitor_speed          = speed;
            visitor_flipped        = eref.flipped();
            eref.extrusion_entity().visit(*this); // will call extrude_thing()
            this->visitor_in_use = false;
            gcode += visitor_gcode;
        }
    }
    return gcode;
}



bool GCodeGenerator::GCodeOutputStream::is_error() const 
{
    return ::ferror(this->f);
}

void GCodeGenerator::GCodeOutputStream::flush()
{
    // allow preproc to flush if they retain strings.
    //std::string str_preproc;
    //m_gcodegen._post_process(str_preproc, true);
    //if (!str_preproc.empty()) {
    //    const char* gcode = str_preproc.c_str();
    //    // writes string to file
    //    fwrite(gcode, 1, ::strlen(gcode), this->f);
    //    //FIXME don't allocate a string, maybe process a batch of lines?
    //    m_processor.process_buffer(std::string(gcode));
    //}
    // flush to file
    ::fflush(this->f);
}

void GCodeGenerator::GCodeOutputStream::close()
{ 
    if (this->f) {
        ::fclose(this->f);
        this->f = nullptr;
    }
}

void GCodeGenerator::GCodeOutputStream::write(const char *what)
{
    if (what != nullptr) {
        //FIXME don't allocate a string, maybe process a batch of lines?
        std::string gcode(m_find_replace ? m_find_replace->process_layer(what) : what);
        if (m_only_ascii) {
            remove_not_ascii(gcode);
        }
        // writes string to file
        fwrite(gcode.c_str(), 1, gcode.size(), this->f);
        m_processor.process_buffer(gcode);
    }
}

void GCodeGenerator::GCodeOutputStream::writeln(const std::string &what)
{
    if (! what.empty())
        this->write(what.back() == '\n' ? what : what + '\n');
}

void GCodeGenerator::GCodeOutputStream::write_format(const char* format, ...)
{
    va_list args;
    va_start(args, format);

    int buflen;
    {
        va_list args2;
        va_copy(args2, args);
        buflen =
    #ifdef _MSC_VER
            ::_vscprintf(format, args2)
    #else
            ::vsnprintf(nullptr, 0, format, args2)
    #endif
            + 1;
        va_end(args2);
    }

    char buffer[1024];
    bool buffer_dynamic = buflen > 1024;
    char *bufptr = buffer_dynamic ? (char*)malloc(buflen) : buffer;
    int res = ::vsnprintf(bufptr, buflen, format, args);
    if (res > 0)
        this->write(bufptr);

    if (buffer_dynamic)
        free(bufptr);

    va_end(args);
}

// TODO: better isnert that into the travel accel/decel
std::string GCodeGenerator::travel_to_first_position(const Vec3crd& point) {
    std::string gcode;

    const Vec3d gcode_point = to_3d(this->point_to_gcode(point.head<2>()), unscaled(point.z()));

    double lift{
        EXTRUDER_CONFIG_WITH_DEFAULT(travel_ramping_lift, false) ? EXTRUDER_CONFIG_WITH_DEFAULT(travel_max_lift, 0.f) :
                                               EXTRUDER_CONFIG_WITH_DEFAULT(retract_lift, 0.f)};
    const double upper_limit = EXTRUDER_CONFIG_WITH_DEFAULT(retract_lift_below ,9999999.f);
    const double lower_limit = EXTRUDER_CONFIG_WITH_DEFAULT(retract_lift_above, 0.f);
    if ((lower_limit > 0 && gcode_point.z() < lower_limit) ||
        (upper_limit > 0 && gcode_point.z() > upper_limit)) {
        lift = 0.0;
    }

    if (EXTRUDER_CONFIG_WITH_DEFAULT(retract_length, 0.f) > 0 && (!this->last_pos_defined() || (!EXTRUDER_CONFIG_WITH_DEFAULT(travel_ramping_lift, false)))) {
        if (!this->last_pos_defined() || EXTRUDER_CONFIG_WITH_DEFAULT(retract_before_travel, false) < (this->point_to_gcode(this->last_pos()) - gcode_point.head<2>()).norm()) {
            gcode += this->writer().retract();
            gcode += this->writer().get_travel_to_z_gcode(gcode_point.z() + lift, "lift");
        }
    }
    this->set_last_pos(point.head<2>());
    this->writer().update_position(gcode_point);

    std::string comment{"move to first layer point"};
    gcode += this->writer().travel_to_xy(gcode_point.head<2>(), 0.0, comment);
    gcode += this->writer().travel_to_z(gcode_point.z(), comment);

    m_current_layer_first_position = gcode_point;
    return gcode;
}


//external_perimeter_cut_corners cache, from 30deg to 145deg (115 deg)
std::vector<double> cut_corner_cache = {
    0.001537451157993,0.001699627500179,0.001873176359929,0.002058542095754,0.002256177154906,0.002466542444994,0.002690107718482,0.002927351970781,0.003178763852686,0.003444842097951,
    0.003726095966834,0.004023045706492,0.004336223029152,0.00466617160904,0.005013447599101,0.005378620168593,0.005762272062727,0.006165000185567,0.006587416207474,0.007030147198493,
    0.007493836289104,0.007979143359902,0.008486745761834,0.009017339068734,0.00957163786399,0.010150376563326,0.010754310275767,0.011384215705013,0.012040892093603,0.012725162212361,
    0.013437873397832,0.01417989864057,0.01495213772733,0.01575551844043,0.016590997817786,0.017459563477334,0.018362235009846,0.019300065444398,0.020274142791089,0.021285591665892,
    0.022335575002924,0.023425295859755,0.024555999321851,0.025728974512639,0.026945556716223,0.028207129620272,0.029515127687218,0.030871038662503,0.032276406229305,0.033732832819934,
    0.035241982594887,0.036805584601441,0.038425436124638,0.040103406244574,0.041841439615055,0.043641560479958,0.045505876945025,0.047436585524337,0.049435975982392,0.051506436494553,
    0.053650459150638,0.055870645828676,0.058169714468295,0.0605505057759,0.063015990396837,0.065569276592991,0.068213618467979,0.070952424786126,0.073789268435947,0.076727896593837,
    0.079772241649261,0.082926432958949,0.086194809504486,0.089581933535469,0.093092605289007,0.096731878886046,0.100505079515854,0.10441782203221,0.108476031098559,0.112685963034856,
    0.117054229536308,0.121587823453898,0.126294146848979,0.131181041559526,0.136256822544454,0.141530314305188,0.147010890721085,0.152708518678027,0.158633805918466,0.164798053597366,
    0.17121331409307,0.17789245469658,0.184849227888721,0.192098349014236,0.199655582277462,0.207537836118677,0.215763269187181,0.224351408310655,0.233323280075731,0.242701557887958,
    0.252510726678311,0.262777267777188,0.27352986689699,0.284799648665007,0.296620441746888,0.309029079319231,0.322065740515038,0.335774339512048,0.350202970204428,0.365404415947691,
    0.381436735764648,0.398363940736199,0.416256777189962,0.435193636891737,0.455261618934834 };

void GCodeGenerator::_extrude_line(std::string& gcode_str, const Line& line, const double e_per_mm, const std::string_view comment, ExtrusionRole role) {
    if (line.a.coincides_with_epsilon(line.b)) {
        assert(false); // todo: investigate if it happens (it happens in perimeters)
        return;
    }
    std::string comment_copy(comment);
    double unscaled_line_length = unscaled(line.length());
    double extrusion_value = e_per_mm * unscaled_line_length;
    // small_area_infill_flow_compensation
    // this is only done in _extrude_line and not in _extrude_line_cut_corner because _extrude_line_cut_corner doesn't apply to solid infill, but only for external perimeters.
    if (!this->on_first_layer() && (role == ExtrusionRole::SolidInfill || role == ExtrusionRole::TopSolidInfill) &&
        m_config.small_area_infill_flow_compensation.value &&
        m_config.small_area_infill_flow_compensation_model.value.data_size() > 1) {
        GraphData graph = m_config.small_area_infill_flow_compensation_model.value;
        assert(graph.begin_idx >= 0 && graph.begin_idx + 1 < graph.end_idx && graph.end_idx <= graph.graph_points.size());
        // ensure it start at length = 0, and ensure it ends with a compensation of 1.
        graph.graph_points[graph.begin_idx].x() = 0;
        graph.graph_points[graph.end_idx - 1].y() = 1;
        //interpolate and verify
        double new_extrusion_value = extrusion_value * graph.interpolate(unscaled_line_length);
        assert(new_extrusion_value > 0.0);
        if (new_extrusion_value != extrusion_value) {
            extrusion_value = (new_extrusion_value > 0.0) ? new_extrusion_value : 0.0;
            if (m_config.gcode_comments) {
                comment_copy += Slic3r::format(_u8L(" | Old Flow Value: %0.5f Length: %0.5f"), extrusion_value, unscaled_line_length);
            }
        }
    }
    // end small_area_infill_flow_compensation
    gcode_str += m_writer.extrude_to_xy(
        this->point_to_gcode(line.b),
        extrusion_value,
        comment_copy);
}

void GCodeGenerator::_extrude_line_cut_corner(std::string& gcode_str, const Line& line, const double e_per_mm, const std::string_view comment, Point& last_pos, const double path_width) {
    {
        if (line.a == line.b) return; //todo: investigate if it happens (it happens in perimeters)
        //check the angle
        assert(ccw_angle_old_test(line.a, last_pos, line.b) == abs_angle(angle_ccw( last_pos - line.a,line.b - line.a)));
        double angle = line.a == last_pos ? PI : abs_angle(angle_ccw( last_pos - line.a,line.b - line.a));
        //convert the angle from the angle of the line to the angle of the "joint" (Circular segment)
        if (angle > PI) angle = angle - PI;
        else angle = PI - angle;
        int idx_angle = int(180 * angle / PI);
        // the coeff is below 0.01 i the angle is higher than 125, so it's not useful
        if (idx_angle > 60) {
            //don't compensate if the angle is under 35, as it's already a 50% compensation, it's enough! 
            if (idx_angle > 144) idx_angle = 144;
            //surface extruded in path.width is path.width * path.width
            // define R = path.width/2 and a = angle/2
            // then i have to print only 4RR + RR(2a-sin(2a))/2 - RR*sina*sina*tana if i want to remove the bits out of the external curve, if the internal overlap go to the exterior.
            // so over RR, i have to multiply the extrudion per 1 + (2a-sin(2a))/8 - (sina*sina*tana)/4
            //double R = scale_(path.width) / 2;
            //double A = (PI - angle) / 2;
            //double added = (A - std::sin(A + A) / 2);
            //double removed = std::sin(A); removed = removed * removed * std::tan(A) / 4;
            //double coeff = 1. + added - removed;
            //we have to remove coeff percentage on path.width length
            double coeff = cut_corner_cache[idx_angle - 30];
            //the length, do half of the work on width/4 and the other half on width/2
            double length1 = (path_width) / 4;
            double line_length = unscaled(line.length());
            if (line_length > length1) {
                double mult1 = 1 - coeff * 2;
                double length2 = (path_width) / 2;
                double mult2 = 1 - coeff;
                double sum = 0;
                //Create a point
                Point inter_point1 = line.point_at(scale_d(length1));
                //extrude very reduced
                gcode_str += this->m_writer.extrude_to_xy(
                    this->point_to_gcode(inter_point1),
                    e_per_mm * (length1)*mult1,
                    comment);
                sum += e_per_mm * (length1)*mult1;

                if (line_length - length1 > length2) {
                    Point inter_point2 = line.point_at(scale_d(length1 + length2));
                    //extrude reduced
                    gcode_str += this->m_writer.extrude_to_xy(
                        this->point_to_gcode(inter_point2),
                        e_per_mm * (length2)*mult2,
                        comment);
                    sum += e_per_mm * (length2)*mult2;

                    //extrude normal
                    gcode_str += this->m_writer.extrude_to_xy(
                        this->point_to_gcode(line.b),
                        e_per_mm * (line_length - (length1 + length2)),
                        comment);
                    sum += e_per_mm * (line_length - (length1 + length2));
                } else {
                    mult2 = 1 - coeff * (length2 / (line_length - length1));
                    gcode_str += this->m_writer.extrude_to_xy(
                        this->point_to_gcode(line.b),
                        e_per_mm * (line_length - length1) * mult2,
                        comment);
                    sum += e_per_mm * (line_length - length1) * mult2;
                }
            } else {
                double mult = std::max(0.1, 1 - coeff * (scale_(path_width) / line_length));
                gcode_str += this->m_writer.extrude_to_xy(
                    this->point_to_gcode(line.b),
                    e_per_mm * line_length * mult,
                    comment);
            }
        } else {
            // nothing special, angle is too shallow to have any impact.
            gcode_str += this->m_writer.extrude_to_xy(
                this->point_to_gcode(line.b),
                e_per_mm * unscaled(line.length()),
                comment);
        }

        // relance
        last_pos = line.a;
    }
}

double GCodeGenerator::_compute_e_per_mm(double path_mm3_per_mm) {
    // no e if no extrusion axis
    if (m_writer.extrusion_axis().empty() || path_mm3_per_mm <= 0)
        return 0;
    // compute
    double e_per_mm = path_mm3_per_mm
        * m_writer.tool()->e_per_mm3() // inside is the filament_extrusion_multiplier
        * this->config().print_extrusion_multiplier.get_abs_value(1);
    // extrusion mult per speed
    if (this->config().extruder_extrusion_multiplier_speed.is_enabled()) {
        GraphData eems_graph = this->config().extruder_extrusion_multiplier_speed.get_at(this->m_writer.tool()->id());
        if (eems_graph.data_size() > 0 && this->config().extruder_extrusion_multiplier_speed.is_enabled(this->m_writer.tool()->id())) {
            assert(e_per_mm > 0);
            double                   current_speed = this->writer().get_speed();
            if (eems_graph.data_size() > 0) {
                e_per_mm *= eems_graph.interpolate(current_speed);
            }
            assert(e_per_mm > 0);
        }
    }
    // first layer mult
    if (this->m_layer->bottom_z() < EPSILON)
        e_per_mm *= this->config().first_layer_flow_ratio.get_abs_value(1);
    return e_per_mm;
}

std::string GCodeGenerator::_extrude(const ExtrusionPath &path, const std::string_view description, double speed) {

    std::string descr = description.empty() ? gcode_extrusion_role_to_string(extrusion_role_to_gcode_extrusion_role(path.role())) : std::string(description);
    std::string gcode = this->_before_extrude(path, descr, speed);

    std::function<void(std::string&, const Line&, double, const std::string&)> func = [this](std::string& gcode, const Line& line, double e_per_mm, const std::string& comment) {
        if (line.a == line.b) return; //todo: investigate if it happens (it happens in perimeters)
        gcode += m_writer.extrude_to_xy(
            this->point_to_gcode(line.b),
            e_per_mm * unscaled(line.length()),
            comment);
    };

    // calculate extrusion length per distance unit
    double e_per_mm = _compute_e_per_mm(path.mm3_per_mm());
    ArcPolyline polyline = path.as_polyline();
    if (polyline.size() > 1) {
        std::string comment = m_config.gcode_comments ? descr : "";

        //BBS: use G1 if not enable arc fitting or has no arc fitting result or in spiral_mode mode
        //Attention: G2 and G3 is not supported in spiral_mode mode
        assert(m_config.arc_fitting != ArcFittingType::Disabled  || !polyline.has_arc());
        if (m_config.arc_fitting == ArcFittingType::Disabled || !polyline.has_arc() || m_config.spiral_vase) {
            assert(!polyline.has_arc());
            Point last_pos    = polyline.front();
            Point current_pos = polyline.front();
            for (size_t idx = 1; idx < polyline.size(); ++idx) {
                if (path.role() != ExtrusionRole::ExternalPerimeter || config().external_perimeter_cut_corners.value == 0) {
                    // normal & legacy pathcode
                    _extrude_line(gcode, Line(current_pos, polyline.get_point(idx)), e_per_mm, comment, path.role());
                } else {
                    _extrude_line_cut_corner(gcode, Line(current_pos, polyline.get_point(idx)), e_per_mm, comment, last_pos, path.width());
                }
                last_pos = current_pos;
                current_pos = polyline.get_point(idx);
            }
        } else if (m_config.arc_fitting == ArcFittingType::Bambu /*bambu arc*/) {
            // BBS: start to generate gcode from arc fitting data which includes line and arc
            Point last_pos    = polyline.front();
            Point current_pos = polyline.front();
            coordf_t radius=  0;
            Point center;
            for (size_t idx = 1; idx < polyline.size(); ++idx) {
                const Geometry::ArcWelder::Segment &segment = polyline.get_arc(idx);
                radius = segment.radius;
                if (radius > 0) {
                    center = Geometry::ArcWelder::arc_center_scalar<coord_t, coordf_t>(current_pos, segment.point, segment.radius, segment.ccw());
                    // Don't extrude a degenerated circle.
                    if (center.coincides_with_epsilon(current_pos))
                        radius = 0;
                }
                if(radius == 0){
                    // strait
                    if (path.role() != ExtrusionRole::ExternalPerimeter || config().external_perimeter_cut_corners.value == 0) {
                        // normal & legacy pathcode
                        _extrude_line(gcode, Line(current_pos, segment.point), e_per_mm, comment, path.role());
                    } else {
                        _extrude_line_cut_corner(gcode, Line(current_pos, segment.point), e_per_mm, comment, last_pos, path.width());
                    }
                } else {
                    const Vec2d  center_offset = this->point_to_gcode(center) - this->point_to_gcode(current_pos);
                    double      angle         = Geometry::ArcWelder::arc_angle(current_pos, segment.point, radius);
                    assert(angle > 0);
                    const coordf_t line_length = angle * std::abs(radius);
                    gcode += m_writer.extrude_arc_to_xy(this->point_to_gcode(segment.point), center_offset, e_per_mm * unscaled(line_length),
                                                        segment.ccw(), comment);
                }
                last_pos    = current_pos;
                current_pos = segment.point;
            }
        } else /*arcwelder*/{
            Geometry::ArcWelder::Path smooth_path = polyline.get_arc();
            if (visitor_flipped)
                Geometry::ArcWelder::reverse(smooth_path);
            Vec2d prev_exact = this->point_to_gcode(smooth_path.front().point);
            Vec2d prev = m_writer.get_default_gcode_formatter().quantize(prev_exact);
            auto  it   = smooth_path.begin();
            auto  end  = smooth_path.end();
            for (++ it; it != end; ++ it) {
                Vec2d p_exact = this->point_to_gcode(it->point);
                Vec2d p       = m_writer.get_default_gcode_formatter().quantize(p_exact);
                assert(p != prev);
                if (p != prev) {
                    // Center of the radius to be emitted into the G-code: Either by radius or by center offset.
                    coordf_t radius = 0;
                    Vec2d  ij;
                    if (it->radius != 0) {
                        // Extrude an arc.
                        assert(m_config.arc_fitting == ArcFittingType::EmitCenter);
                        radius = unscaled<coordf_t>(it->radius);
                        {
                            // Calculate quantized IJ circle center offset.
                            ij = m_writer.get_default_gcode_formatter().quantize(
                                Vec2d(
                                    Geometry::ArcWelder::arc_center(prev_exact.cast<coordf_t>(), p_exact.cast<coordf_t>(), coordf_t(radius), it->ccw())
                                    - prev));
                            if (ij == Vec2d::Zero())
                                // Don't extrude a degenerated circle.
                                radius = 0;
                        }
                    }
                    if (radius == 0) {
                        // Extrude line segment.
                        if (const double line_length = (p - prev).norm(); line_length > 0) {
                            //path_length += line_length;
                            gcode += m_writer.extrude_to_xy(p, e_per_mm * line_length, comment);
                        }
                    } else {
                        double angle = Geometry::ArcWelder::arc_angle(prev.cast<coordf_t>(), p.cast<coordf_t>(), coordf_t(radius));
                        assert(angle > 0);
                        const double line_length = angle * std::abs(radius);
                        //path_length += line_length;
                        const double dE = e_per_mm * line_length;
                        assert(dE > 0);
                        gcode += m_writer.extrude_arc_to_xy(p, ij, it->ccw(), dE, comment);
                    }
                    prev = p;
                    prev_exact = p_exact;
                }
            }
        }
    }
    gcode += this->_after_extrude(path);

    return gcode;
}

double_t GCodeGenerator::_compute_speed_mm_per_sec(const ExtrusionPath& path, double speed, double &fan_speed, std::string *comment) {

    float factor = 1;
    // set speed
    if (speed < 0) {
        //if speed == -1, then it's means "choose yourself, but if it's < SMALL_PERIMETER_SPEED_RATIO_OFFSET, then it's a scaling from small_perimeter.
        if (speed <= SMALL_PERIMETER_SPEED_RATIO_OFFSET) {
            factor = float(-speed + SMALL_PERIMETER_SPEED_RATIO_OFFSET);
        }
        //it's a bit hacky, so if you want to rework it, help yourself.
        if (path.role() == ExtrusionRole::Perimeter) {
            speed = m_config.get_computed_value("perimeter_speed");
            if(comment) *comment = "perimeter_speed";
        } else if (path.role() == ExtrusionRole::ExternalPerimeter) {
            speed = m_config.get_computed_value("external_perimeter_speed");
            if(comment) *comment = "external_perimeter_speed";
        } else if (path.role() == ExtrusionRole::BridgeInfill) {
            speed = m_config.get_computed_value("bridge_speed");
            if(comment) *comment = "bridge_speed";
        } else if (path.role() == ExtrusionRole::InternalBridgeInfill) {
            speed = m_config.get_computed_value("internal_bridge_speed");
            if(comment) *comment = "internal_bridge_speed";
        } else if (path.role().has(ExtrusionRole::OverhangPerimeter)) { // OverhangPerimeter or OverhangExternalPerimeter
            speed = m_config.get_computed_value("overhangs_speed");
            if(comment) *comment = "overhangs_speed";
        } else if (path.role() == ExtrusionRole::InternalInfill) {
            speed = m_config.get_computed_value("infill_speed");
            if(comment) *comment = "infill_speed";
        } else if (path.role() == ExtrusionRole::SolidInfill) {
            speed = m_config.get_computed_value("solid_infill_speed");
            if(comment) *comment = "solid_infill_speed";
        } else if (path.role() == ExtrusionRole::TopSolidInfill) {
            speed = m_config.get_computed_value("top_solid_infill_speed");
            if(comment) *comment = "top_solid_infill_speed";
        } else if (path.role() == ExtrusionRole::ThinWall) {
            speed = m_config.get_computed_value("thin_walls_speed");
            if(comment) *comment = "thin_walls_speed";
        } else if (path.role() == ExtrusionRole::GapFill) {
            speed = m_config.get_computed_value("gap_fill_speed");
            if(comment) *comment = "gap_fill_speed";
            double max_ratio = m_config.gap_fill_flow_match_perimeter.get_abs_value(1.);
            if (max_ratio > 0 && m_region) {
                //compute intended perimeter flow
                Flow fl = m_region->flow(*m_layer->object(), FlowRole::frPerimeter, m_layer->height, m_layer->id());
                double max_vol_speed = fl.mm3_per_mm() * max_ratio * m_config.get_computed_value("perimeter_speed");
                double current_vol_speed = path.mm3_per_mm() * speed;
                if (max_vol_speed < current_vol_speed) {
                    speed = max_vol_speed / path.mm3_per_mm();
                    if(comment) *comment = "max_vol_speed (from " + (*comment) + ")";
                }
            }
        } else if (path.role() == ExtrusionRole::Ironing) {
            speed = m_config.get_computed_value("ironing_speed");
            if(comment) *comment = "ironing_speed";
        } else if (path.role() == ExtrusionRole::None || path.role() == ExtrusionRole::Travel) {
            assert(path.role() != ExtrusionRole::None);
            speed = m_config.get_computed_value("travel_speed");
            if(comment) *comment = "travel_speed";
        } else if (path.role() == ExtrusionRole::Milling) {
            speed = m_config.get_computed_value("milling_speed");
            if(comment) *comment = "milling_speed";
        } else if (path.role() == ExtrusionRole::SupportMaterial) {
            speed = m_config.get_computed_value("support_material_speed");
            if(comment) *comment = "support_material_speed";
        } else if (path.role() == ExtrusionRole::SupportMaterialInterface) {
            speed = m_config.get_computed_value("support_material_interface_speed");
            if(comment) *comment = "support_material_interface_speed";
        } else if (path.role() == ExtrusionRole::Skirt) {
            speed = m_config.get_computed_value("brim_speed");
            if(comment) *comment = "brim_speed";
        } else {
            throw Slic3r::InvalidArgument("Invalid speed");
        }
    } else {
        if (comment) *comment = "previous speed";
    }
    const std::string comment_auto_speed = "(from autospeed)";
    if (m_volumetric_speed != 0. && speed == 0) {
        //if m_volumetric_speed, use the max size for thinwall & gapfill, to avoid variations
        double vol_speed = m_volumetric_speed / path.mm3_per_mm();
        double max_print_speed = m_config.get_computed_value("max_print_speed");
        if (vol_speed > max_print_speed) {
            vol_speed = max_print_speed;
            if(comment) *comment = std::string("% of max_volumetric_speed limited by max_print_speed") + std::to_string(vol_speed);
        } else {
            if(comment) *comment = std::string("% of max_volumetric_speed ") + std::to_string(vol_speed);
        }

        // if using a % of an auto speed, use the % over the volumetric speed.
        if (path.role() == ExtrusionRole::Perimeter) {
            speed = m_config.perimeter_speed.get_abs_value(vol_speed);
            if(comment) *comment = std::string("perimeter_speed ") + *comment;
        } else if (path.role() == ExtrusionRole::ExternalPerimeter) {
            speed = m_config.external_perimeter_speed.get_abs_value(vol_speed);
            if(comment) *comment = std::string("external_perimeter_speed ") + *comment;
        } else if (path.role() == ExtrusionRole::BridgeInfill) {
            speed = m_config.bridge_speed.get_abs_value(vol_speed);
            if(comment) *comment = std::string("bridge_speed ") + *comment;
        } else if (path.role() == ExtrusionRole::InternalBridgeInfill) {
            speed = m_config.internal_bridge_speed.get_abs_value(vol_speed);
            if(comment) *comment = std::string("internal_bridge_speed ") + *comment;
        } else if (path.role().has(ExtrusionRole::OverhangPerimeter)) { // also overhang external perimeter
            speed = m_config.overhangs_speed.get_abs_value(vol_speed);
            if(comment) *comment = std::string("overhangs_speed ") + *comment;
        } else if (path.role() == ExtrusionRole::InternalInfill) {
            speed = m_config.infill_speed.get_abs_value(vol_speed);
            if(comment) *comment = std::string("infill_speed ") + *comment;
        } else if (path.role() == ExtrusionRole::SolidInfill) {
            speed = m_config.solid_infill_speed.get_abs_value(vol_speed);
            if(comment) *comment = std::string("solid_infill_speed ") + *comment;
        } else if (path.role() == ExtrusionRole::TopSolidInfill) {
            speed = m_config.top_solid_infill_speed.get_abs_value(vol_speed);
            if(comment) *comment = std::string("top_solid_infill_speed ") + *comment;
        } else if (path.role() == ExtrusionRole::ThinWall) {
            speed = m_config.thin_walls_speed.get_abs_value(vol_speed);
            if(comment) *comment = std::string("thin_walls_speed ") + *comment;
        } else if (path.role() == ExtrusionRole::GapFill) {
            speed = m_config.gap_fill_speed.get_abs_value(vol_speed);
            if(comment) *comment = std::string("gap_fill_speed ") + *comment;
        } else if (path.role() == ExtrusionRole::Ironing) {
            speed = m_config.ironing_speed.get_abs_value(vol_speed);
            if(comment) *comment = std::string("ironing_speed ") + *comment;
        }
        if (speed == 0) {
            speed = vol_speed;
            if(comment) *comment = "max_volumetric_speed";
            if (vol_speed > max_print_speed)
                if(comment) *comment += " limited by max_print_speed";
        }
    }
    if (speed == 0) { // if you don't have a m_volumetric_speed
        speed = m_config.max_print_speed.value;
        if(comment) *comment = "max_print_speed";
    }
    // Apply small perimeter 'modifier
    //  don't modify bridge speed
    if (factor < 1 && !path.role().is_bridge()) {
        float small_speed = (float)m_config.small_perimeter_speed.get_abs_value(m_config.get_computed_value("perimeter_speed"));
        if (small_speed > 0) {
            // apply factor between feature speed and small speed
            speed = (speed * factor) + double((1.f - factor) * small_speed);
            if(comment) *comment += ", reduced by small_perimeter_speed";
        }
    }
    // Apply first layer modifier
    if (this->on_first_layer()) {
        const double base_speed = speed;
        double first_layer_speed = m_config.first_layer_speed.get_abs_value(base_speed);
        if (path.role() == ExtrusionRole::InternalInfill || path.role() == ExtrusionRole::SolidInfill) {
            double first_layer_infill_speed = m_config.first_layer_infill_speed.get_abs_value(base_speed);
            if (first_layer_infill_speed > 0) {
                if (first_layer_infill_speed < speed) {
                    speed = first_layer_infill_speed;
                    if(comment) *comment += ", reduced to first_layer_infill_speed";
                }
            } else if (first_layer_speed > 0) {
                if (first_layer_speed < speed) {
                    speed = first_layer_speed;
                    if(comment) *comment += ", reduced to first_layer_speed";
                }
            }
        } else {
            if (first_layer_speed > 0 && first_layer_speed < speed) {
                speed = first_layer_speed;
                if(comment) *comment += ", reduced to first_layer_speed";
            }
        }
        double first_layer_min_speed = m_config.first_layer_min_speed.value;
        if (first_layer_min_speed > speed) {
            speed = first_layer_min_speed;
            if(comment) *comment += ", increased to first_layer_min_speed";
        }
    } else if (this->object_layer_over_raft()) {
        const double base_speed = speed;
        double first_layer_over_raft_speed = m_config.first_layer_speed_over_raft.get_abs_value(base_speed);
        if (first_layer_over_raft_speed > 0 && first_layer_over_raft_speed < speed) {
            speed = first_layer_over_raft_speed;
            if(comment) *comment += ", reduced to first_layer_over_raft_speed";
        }
    }

    // the first_layer_flow_ratio is added at the last time to take into account everything. So do the compute like it's here.
    double path_mm3_per_mm = path.mm3_per_mm();
    if (m_layer->bottom_z() < EPSILON) {
        path_mm3_per_mm *= this->config().first_layer_flow_ratio.get_abs_value(1);
    }
    // cap speed with max_volumetric_speed anyway (even if user is not using autospeed)
    if (m_config.max_volumetric_speed.value > 0 && path_mm3_per_mm > 0 && m_config.max_volumetric_speed.value / path_mm3_per_mm < speed) {
        speed = m_config.max_volumetric_speed.value / path_mm3_per_mm;
        if(comment) *comment += ", reduced by max_volumetric_speed";
    }
    // filament cap (volumetric & raw speed)
    double filament_max_volumetric_speed = EXTRUDER_CONFIG_WITH_DEFAULT(filament_max_volumetric_speed, 0);
    if (filament_max_volumetric_speed > 0 && path_mm3_per_mm > 0 && filament_max_volumetric_speed / path_mm3_per_mm < speed) {
        speed = filament_max_volumetric_speed / path_mm3_per_mm;
        if(comment) *comment += ", reduced by filament_max_volumetric_speed";
    }
    double filament_max_speed = EXTRUDER_CONFIG_WITH_DEFAULT(filament_max_speed, 0);
    if (filament_max_speed > 0 && filament_max_speed < speed) {
        speed = filament_max_speed;
        if(comment) *comment += ", reduced by filament_max_speed";
    }
    
    // compute for enable_dynamic_overhang_speeds & overhang_speed_0 1 2 3
    std::pair<float, float> dynamic_speed_and_fan_speed{-1, -1};
    if (path.attributes().overhang_attributes.has_value()) {
        double external_perim_reference_speed = m_config.get_computed_value("external_perimeter_speed");
        if (external_perim_reference_speed == 0)
            external_perim_reference_speed = m_volumetric_speed / path.mm3_per_mm();
        // apply again all min/max/mod on external_perim_reference_speed
        if (m_layer->bottom_z() < EPSILON)
            external_perim_reference_speed *= this->config().first_layer_flow_ratio.get_abs_value(1);
        if (m_config.max_volumetric_speed.value > 0)
            external_perim_reference_speed = std::min(external_perim_reference_speed,
                                                      m_config.max_volumetric_speed.value / path.mm3_per_mm());
        if (EXTRUDER_CONFIG_WITH_DEFAULT(filament_max_volumetric_speed, 0) > 0) {
            external_perim_reference_speed = std::min(external_perim_reference_speed,
                                                      filament_max_volumetric_speed / path.mm3_per_mm());
        }
        if (filament_max_speed > 0) {
            external_perim_reference_speed = std::min(filament_max_speed, external_perim_reference_speed);
        }
        assert(m_writer.tool());
        dynamic_speed_and_fan_speed = ExtrusionProcessor::calculate_overhang_speed(path.attributes(),
            this->m_config, m_writer.tool()->id(), external_perim_reference_speed, speed);
    }
    if (dynamic_speed_and_fan_speed.first >= 0) {
        speed = dynamic_speed_and_fan_speed.first;
        if (dynamic_speed_and_fan_speed.first >= 0)
            fan_speed = dynamic_speed_and_fan_speed.second;
    }

    return speed;
}

std::pair<double, double> GCodeGenerator::_compute_acceleration(const ExtrusionPath& path)
{
    // adjust acceleration, inside the travel to set the deceleration (unless it's deactivated)
    double acceleration = get_default_acceleration(m_config);
    double max_acceleration = std::numeric_limits<double>::max();
    // on 2.3, check for enable/disable if(config.machine_limits_usage)
    if (m_config.machine_limits_usage <= MachineLimitsUsage::Limits)
        max_acceleration = m_config.machine_max_acceleration_extruding.get_at(0);
    double travel_acceleration = get_travel_acceleration(m_config);
    if (acceleration > 0) {
        switch (extrusion_role_to_gcode_extrusion_role(path.role())){
            case GCodeExtrusionRole::Perimeter:
            perimeter:
                if (m_config.perimeter_acceleration.value > 0) {
                    double perimeter_acceleration = m_config.get_computed_value("perimeter_acceleration");
                    if (perimeter_acceleration > 0)
                        acceleration = perimeter_acceleration;
                }
                break;
            case GCodeExtrusionRole::ExternalPerimeter:
            externalPerimeter:
                if (m_config.external_perimeter_acceleration.value > 0) {
                    double external_perimeter_acceleration = m_config.get_computed_value("external_perimeter_acceleration");
                    if (external_perimeter_acceleration > 0) {
                        acceleration = external_perimeter_acceleration;
                        break;
                    }
                }
                goto perimeter;
            case GCodeExtrusionRole::SolidInfill:
            solidInfill:
                if (m_config.solid_infill_acceleration.value > 0) {
                    double solid_infill_acceleration = m_config.get_computed_value("solid_infill_acceleration");
                    if (solid_infill_acceleration > 0)
                        acceleration = solid_infill_acceleration;
                }
                break;
            case GCodeExtrusionRole::InternalInfill:
            //internalInfill:
                if (m_config.infill_acceleration.value > 0) {
                    double infill_acceleration = m_config.get_computed_value("infill_acceleration");
                    if (infill_acceleration > 0) {
                        acceleration = infill_acceleration;
                        break;
                    }
                }
                goto solidInfill;
            case GCodeExtrusionRole::TopSolidInfill:
            topSolidInfill:
                if (m_config.top_solid_infill_acceleration.value > 0) {
                    double top_solid_infill_acceleration = m_config.get_computed_value("top_solid_infill_acceleration");
                    if (top_solid_infill_acceleration > 0) {
                        acceleration = top_solid_infill_acceleration;
                        break;
                    }
                }
                goto solidInfill;
            case GCodeExtrusionRole::Ironing:
                if (m_config.ironing_acceleration.value > 0) {
                    double ironing_acceleration = m_config.get_computed_value("ironing_acceleration");
                    if (ironing_acceleration > 0) {
                        acceleration = ironing_acceleration;
                        break;
                    }
                }
                goto topSolidInfill;
            case GCodeExtrusionRole::SupportMaterial:
            case GCodeExtrusionRole::WipeTower:
            supportMaterial:
                if (m_config.support_material_acceleration.value > 0) {
                    double support_material_acceleration = m_config.get_computed_value("support_material_acceleration");
                    if (support_material_acceleration > 0)
                        acceleration = support_material_acceleration;
                }
                break;
            case GCodeExtrusionRole::SupportMaterialInterface:
                if (m_config.support_material_interface_acceleration.value > 0) {
                    double support_material_interface_acceleration = m_config.get_computed_value("support_material_interface_acceleration");
                    if (support_material_interface_acceleration > 0) {
                        acceleration = support_material_interface_acceleration;
                        break;
                    }
                }
                goto supportMaterial;
            case GCodeExtrusionRole::Skirt:
                //skirtBrim:
                if (m_config.brim_acceleration.value > 0) {
                    double brim_acceleration = m_config.get_computed_value("brim_acceleration");
                    if (brim_acceleration > 0) {
                        acceleration = brim_acceleration;
                        break;
                    }
                }
                goto supportMaterial;
            case GCodeExtrusionRole::BridgeInfill:
            bridgeInfill:
                if (m_config.bridge_acceleration.value > 0) {
                    double bridge_acceleration = m_config.get_computed_value("bridge_acceleration");
                    if (bridge_acceleration > 0)
                        acceleration = bridge_acceleration;
                }
                break;
            case GCodeExtrusionRole::InternalBridgeInfill:
                if (m_config.internal_bridge_acceleration.value > 0) {
                    double internal_bridge_acceleration = m_config.get_computed_value("internal_bridge_acceleration");
                    if (internal_bridge_acceleration > 0) {
                        acceleration = internal_bridge_acceleration;
                        break;
                    }
                }
                goto bridgeInfill;
            case GCodeExtrusionRole::OverhangPerimeter:
                if (m_config.overhangs_acceleration.value > 0) {
                    double overhangs_acceleration = m_config.get_computed_value("overhangs_acceleration");
                    if (overhangs_acceleration > 0) {
                        acceleration = overhangs_acceleration;
                        break;
                    }
                }
                goto bridgeInfill;
            case GCodeExtrusionRole::GapFill:
                if (m_config.gap_fill_acceleration.value > 0) {
                    double gap_fill_acceleration = m_config.get_computed_value("gap_fill_acceleration");
                    if (gap_fill_acceleration > 0) {
                        acceleration = gap_fill_acceleration;
                        break;
                    }
                }
                goto perimeter;
                break;
            case GCodeExtrusionRole::ThinWall:
                if (m_config.thin_walls_acceleration.value > 0) {
                    double thin_walls_acceleration = m_config.get_computed_value("thin_walls_acceleration");
                    if (thin_walls_acceleration > 0) {
                        acceleration = thin_walls_acceleration;
                        break;
                    }
                }
                goto externalPerimeter;
            case GCodeExtrusionRole::None:
                assert(false);
            case GCodeExtrusionRole::Milling:
            case GCodeExtrusionRole::Custom:
            case GCodeExtrusionRole::Travel:
            default:
                break;
        }

        if (this->on_first_layer() && m_config.first_layer_acceleration.value > 0) {
            acceleration = std::min(acceleration, m_config.first_layer_acceleration.get_abs_value(acceleration));
        } else if (this->object_layer_over_raft() && m_config.first_layer_acceleration_over_raft.value > 0) {
            acceleration = m_config.first_layer_acceleration_over_raft.get_abs_value(acceleration);
        }

        acceleration = std::min(max_acceleration, acceleration);

    }
    return {acceleration, travel_acceleration};
}

void GCodeGenerator::cooldown_marker_init() {
    if (!_cooldown_marker_speed[uint8_t(GCodeExtrusionRole::ExternalPerimeter)].empty()) {
        std::string allow_speed_change = ";_EXTRUDE_SET_SPEED";
        //only change speed on external perimeter (and similar) speed if really necessary.
        std::string maybe_allow_speed_change = ";_EXTRUDE_SET_SPEED_MAYBE";
        _cooldown_marker_speed[uint8_t(GCodeExtrusionRole::None)]                 = "";
        _cooldown_marker_speed[uint8_t(GCodeExtrusionRole::Perimeter)]            = allow_speed_change;
        _cooldown_marker_speed[uint8_t(GCodeExtrusionRole::ExternalPerimeter)]    = maybe_allow_speed_change;
        _cooldown_marker_speed[uint8_t(GCodeExtrusionRole::OverhangPerimeter)]    = "";
        _cooldown_marker_speed[uint8_t(GCodeExtrusionRole::InternalInfill)]       = allow_speed_change;
        _cooldown_marker_speed[uint8_t(GCodeExtrusionRole::SolidInfill)]          = allow_speed_change;
        _cooldown_marker_speed[uint8_t(GCodeExtrusionRole::TopSolidInfill)]       = allow_speed_change;
        _cooldown_marker_speed[uint8_t(GCodeExtrusionRole::Ironing)]              = maybe_allow_speed_change;
        _cooldown_marker_speed[uint8_t(GCodeExtrusionRole::BridgeInfill)]         = "";
        _cooldown_marker_speed[uint8_t(GCodeExtrusionRole::InternalBridgeInfill)] = maybe_allow_speed_change;
        _cooldown_marker_speed[uint8_t(GCodeExtrusionRole::ThinWall)]             = maybe_allow_speed_change;
        _cooldown_marker_speed[uint8_t(GCodeExtrusionRole::GapFill)]              = allow_speed_change;
        _cooldown_marker_speed[uint8_t(GCodeExtrusionRole::Skirt)]                = allow_speed_change;
        _cooldown_marker_speed[uint8_t(GCodeExtrusionRole::SupportMaterial)]      = allow_speed_change;
        _cooldown_marker_speed[uint8_t(GCodeExtrusionRole::SupportMaterialInterface)] = maybe_allow_speed_change;
        _cooldown_marker_speed[uint8_t(GCodeExtrusionRole::WipeTower)]                = allow_speed_change;
        _cooldown_marker_speed[uint8_t(GCodeExtrusionRole::Milling)]                  = "";
        _cooldown_marker_speed[uint8_t(GCodeExtrusionRole::Custom)]                   = maybe_allow_speed_change;
        _cooldown_marker_speed[uint8_t(GCodeExtrusionRole::Travel)]                   = maybe_allow_speed_change;
    }
}

std::string GCodeGenerator::_before_extrude(const ExtrusionPath &path, const std::string_view description_in, double speed) {
    std::string gcode;
    gcode.reserve(512);
    std::string description{ description_in };

    auto [/*double*/acceleration, /*double*/travel_acceleration] = _compute_acceleration(path);
    // compute speed here to be able to know it for travel_deceleration_use_target
    std::string speed_comment = "";
    speed = _compute_speed_mm_per_sec(path, speed, m_last_fan_override, m_config.gcode_comments ? &speed_comment : nullptr);
        
    if (m_config.travel_deceleration_use_target) {
        if (!m_current_layer_first_position) {
            // TODO: use travel acceleration, travel_deceleration_use_target
            const Vec3crd point = to_3d(path.first_point(), scale_t(this->m_last_layer_z));
            gcode += this->travel_to_first_position(point);
        } else if (travel_acceleration <= acceleration || travel_acceleration == 0 || acceleration == 0) {
            m_writer.set_travel_acceleration((uint32_t)floor(acceleration + 0.5));
            m_writer.set_acceleration((uint32_t)floor(acceleration + 0.5));
            // go to first point of extrusion path (stop at midpoint to let us set the decel speed)
            if (!last_pos_defined() || last_pos() != path.first_point()) {
                Polyline polyline = this->travel_to(gcode, path.first_point(), path.role());
                this->write_travel_to(gcode, polyline, "move to first " + description + " point");
            }
        } else {
            // go to midpoint to let us set the decel speed)
            if (!last_pos_defined() || last_pos() != path.first_point()) {
                Polyline poly_start = this->travel_to(gcode, path.first_point(), path.role());
                coordf_t length = poly_start.length();
                // compute some numbers
                double previous_accel = m_writer.get_acceleration(); // in mm/s²
                double previous_speed = m_writer.get_speed(); // in mm/s
                double travel_speed = m_config.get_computed_value("travel_speed");
                // first, the acceleration distance
                const double extrude2travel_speed_diff = previous_speed >= travel_speed ? 0 : (travel_speed - previous_speed);
                const double seconds_to_go_travel_speed = (extrude2travel_speed_diff / travel_acceleration);
                const coordf_t dist_to_go_travel_speed = scaled(seconds_to_go_travel_speed * (travel_speed - extrude2travel_speed_diff/2));
                assert(dist_to_go_travel_speed >= 0);
                assert(!std::isinf(dist_to_go_travel_speed));
                assert(!std::isnan(dist_to_go_travel_speed));
                // then the deceleration distance
                const double travel2extrude_speed_diff = speed >= travel_speed ? 0 : (travel_speed - speed);
                const double seconds_to_go_extrude_speed = (travel2extrude_speed_diff / acceleration);
                const coordf_t dist_to_go_extrude_speed = scaled(seconds_to_go_extrude_speed * (travel_speed - travel2extrude_speed_diff / 2));
                assert(dist_to_go_extrude_speed >= 0);
                assert(!std::isinf(dist_to_go_extrude_speed));
                assert(!std::isnan(dist_to_go_extrude_speed));
                // acceleration to go from previous speed to the new one without going by the travel speed
                const double extrude2extrude_speed_diff = std::abs(previous_speed - speed);
                const double accel_extrude2extrude = extrude2extrude_speed_diff * (previous_speed + speed) / (2 * length);
                assert(dist_to_go_extrude_speed >= 0);
                assert(!std::isinf(accel_extrude2extrude));
                assert(!std::isnan(accel_extrude2extrude));
                // check if using a deceleration is useful
                // can't use it if no previous pos
                bool cant_use_deceleration = !last_pos_defined();
                // don't use it if the distance is too small
                coordf_t min_dist_for_deceleration = coordf_t(SCALED_EPSILON);
                min_dist_for_deceleration = std::max(min_dist_for_deceleration, dist_to_go_extrude_speed / 10);
                min_dist_for_deceleration = std::max(min_dist_for_deceleration, scale_d(m_config.min_length));
                cant_use_deceleration = cant_use_deceleration || length < min_dist_for_deceleration;
                // don't use it their isn't enough acceleration to go to the next speed without going by the travel speed
                cant_use_deceleration = cant_use_deceleration || accel_extrude2extrude * 1.1 > acceleration;
                // don't use it if the travel speed isn't high enough vs next speed
                cant_use_deceleration = cant_use_deceleration || dist_to_go_extrude_speed < coordf_t(SCALED_EPSILON);
                if (cant_use_deceleration) {
                    m_writer.set_travel_acceleration((uint32_t)floor(acceleration + 0.5));
                    m_writer.set_acceleration((uint32_t)floor(acceleration + 0.5));
                    this->write_travel_to(gcode, poly_start, "move to first " + description + " point (minimum acceleration)");
                } else {
                    // if length is enough, it's not the hack for first move, and the travel accel is different than the normal accel
                    // then cut the travel in two to change the accel in-between
                    // TODO: compute the real point where it should be cut, considering an infinite max speed.
                        Polyline poly_end;
                        const coordf_t needed_decel_length = dist_to_go_extrude_speed + min_dist_for_deceleration;
                        if (poly_start.size() > 2 && length > dist_to_go_travel_speed + needed_decel_length) {
                            //if complex travel, try to deccelerate only at the end, unless it's less than ~ 20 nozzle
                            if (poly_start.lines().back().length() < needed_decel_length) {
                                poly_end = poly_start;
                                poly_start.clip_end(needed_decel_length);
                                poly_end.clip_start(length - needed_decel_length);
                            } else {
                                poly_end.points.push_back(poly_start.points.back());
                                poly_start.points.pop_back();
                                poly_end.points.push_back(poly_start.points.back());
                                poly_end.reverse();
                            }
                        } else {
                            // simple & not long enough travel : split at the point of inflexion
                            double ratio = (dist_to_go_travel_speed + 1) / (dist_to_go_travel_speed + dist_to_go_extrude_speed + 1);
                            poly_end = poly_start;
                            poly_start.clip_end(length  * ratio);
                            poly_end.clip_start(length * (1-ratio));
                        }
                        gcode += "; acceleration to travel\n";
                        m_writer.set_travel_acceleration((uint32_t)floor(travel_acceleration + 0.5));
                        this->write_travel_to(gcode, poly_start, "move to first " + description + " point (acceleration)");
                        //travel acceleration should be already set at startup via special gcode, and so it's automatically used by G0.
                        gcode += "; decel to extrusion\n";
                        m_writer.set_travel_acceleration((uint32_t)floor(acceleration + 0.5));
                        this->write_travel_to(gcode, poly_end, "move to first " + description + " point (deceleration)");
                        // restore travel accel and ensure the new extrusion accel is set
                        m_writer.set_travel_acceleration((uint32_t)floor(travel_acceleration + 0.5));
                        m_writer.set_acceleration((uint32_t)floor(acceleration + 0.5));
                        gcode += "; end travel\n";
                }
            } else {
                m_writer.set_acceleration((uint32_t)floor(acceleration + 0.5));
            }
        }
    } else {
      if (!m_current_layer_first_position) {
        const Vec3crd point = to_3d(path.first_point(), scaled(this->m_last_layer_z));
        gcode += this->travel_to_first_position(point);
      } else {
        if (!last_pos_defined() || last_pos() != path.first_point()) {
            m_writer.set_travel_acceleration((uint32_t)floor(travel_acceleration + 0.5));
            Polyline polyline = this->travel_to(gcode, path.first_point(), path.role());
            this->write_travel_to(gcode, polyline, "move to first " + description + " point");
            m_writer.set_acceleration((uint32_t)floor(acceleration + 0.5));
        } else {
            m_writer.set_acceleration((uint32_t)floor(acceleration + 0.5));
        }
      }
    }

    //if needed, write the gcode_label_objects_end then gcode_label_objects_start
    //should be already done by travel_to, but just in case
    _add_object_change_labels(gcode);

    // compensate retraction
    if (m_delayed_layer_change.empty()) {
        gcode += m_writer.unlift();//this->unretract();
    } else {
        //check if an unlift happens
        std::string unlift = m_writer.unlift();
        if (unlift.empty()) {
            unlift = m_delayed_layer_change;
        }
        m_delayed_layer_change.clear();
        gcode += unlift;
    }
    gcode += m_writer.unretract();

    if (!m_pending_pre_extrusion_gcode.empty()) {
        // There is G-Code that is due to be inserted before an extrusion starts. Insert it.
        gcode += m_pending_pre_extrusion_gcode;
        m_pending_pre_extrusion_gcode.clear();
    }
    // extrude arc or line
    GCodeExtrusionRole grole = extrusion_role_to_gcode_extrusion_role(path.role());
    if (grole != m_last_extrusion_role && !m_config.feature_gcode.value.empty()) {
        DynamicConfig config;
        config.set_key_value("extrusion_role", new ConfigOptionString(gcode_extrusion_role_to_string(grole)));
        config.set_key_value("next_extrusion_role", new ConfigOptionString(gcode_extrusion_role_to_string(grole)));
        config.set_key_value("previous_extrusion_role", new ConfigOptionString(gcode_extrusion_role_to_string(m_last_extrusion_role)));
        config.set_key_value("last_extrusion_role", new ConfigOptionString(gcode_extrusion_role_to_string(m_last_extrusion_role)));
        config.set_key_value("layer_num", new ConfigOptionInt(m_layer_index + 1));
        config.set_key_value("layer_z", new ConfigOptionFloat(m_layer == nullptr ? m_last_height : m_layer->print_z));
        config.set_key_value("max_layer_z", new ConfigOptionFloat(m_max_layer_z));
        gcode += this->placeholder_parser_process("feature_gcode", m_config.feature_gcode.value,
                                                  m_writer.tool()->id(), &config)
            + "\n";
    }
    if (m_enable_extrusion_role_markers) {
        if (grole != m_last_extrusion_role) {
            char buf[32];
            sprintf(buf, ";_EXTRUSION_ROLE:%d\n", int(grole));
            gcode += buf;
        }
    }
    m_last_extrusion_role = grole;

    // adds processor tags and updates processor tracking data
    // PrusaMultiMaterial::Writer may generate GCodeProcessor::Height_Tag lines without updating m_last_height
    // so, if the last role was GCodeExtrusionRole::WipeTower we force export of GCodeProcessor::Height_Tag lines
    bool last_was_wipe_tower = (m_last_processor_extrusion_role == GCodeExtrusionRole::WipeTower);
    assert(is_decimal_separator_point());

    if (grole != m_last_processor_extrusion_role) {
        m_last_processor_extrusion_role = grole;
        char buf[64];
        sprintf(buf, ";%s%s\n", GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Role).c_str(),
            gcode_extrusion_role_to_string(grole).c_str());
        gcode += buf;
    }

    if (last_was_wipe_tower || m_last_width != path.width()) {
        m_last_width = path.width();
        gcode += std::string(";") + GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Width)
               + float_to_string_decimal_point(m_last_width) + "\n";
    }

#if ENABLE_GCODE_VIEWER_DATA_CHECKING
    if (last_was_wipe_tower || (m_last_mm3_per_mm != path.mm3_per_mm())) {
        m_last_mm3_per_mm = path.mm3_per_mm();
        gcode += std::string(";") + GCodeProcessor::Mm3_Per_Mm_Tag
            + float_to_string_decimal_point(m_last_mm3_per_mm) + "\n";
    }
#endif // ENABLE_GCODE_VIEWER_DATA_CHECKING

    if (last_was_wipe_tower || std::abs(m_last_height - path.height()) > EPSILON) {
        m_last_height = path.height();

        gcode += std::string(";") + GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Height)
            + float_to_string_decimal_point(m_last_height) + "\n";
    }

    std::string cooling_marker_setspeed_comments;
    assert(grole > GCodeExtrusionRole::None);
    assert(grole < GCodeExtrusionRole::Count);
    if (m_enable_cooling_markers) {
        // Send the current extrusion type to Coolingbuffer
        gcode += ";_EXTRUDETYPE_"; gcode += char('A' + uint8_t(grole)); gcode += "\n";
        // comment to be on the same line as the speed command.
        cooling_marker_setspeed_comments = GCodeGenerator::_cooldown_marker_speed[uint8_t(grole)];
    }
    // F     is mm per minute.
    // speed is mm per second
    gcode += m_writer.set_speed(speed, speed_comment, cooling_marker_setspeed_comments);

    if (m_last_fan_override >= 0)
        gcode += ";_SET_FAN_SPEED" + std::to_string(int(m_last_fan_override)) + "\n";
    
    return gcode;
}

std::string GCodeGenerator::_after_extrude(const ExtrusionPath &path) {
    std::string gcode;
    if (m_enable_cooling_markers) {
        // Notify Coolingbuffer that the current extrusion end.
        gcode += ";_EXTRUDE_END\n";
    }

    if (path.role() != ExtrusionRole::GapFill ) {
        m_last_notgapfill_extrusion_role = extrusion_role_to_gcode_extrusion_role(path.role());
    }
    
    if (m_last_fan_override >= 0) {
        gcode += ";_RESET_FAN_SPEED\n";
        m_last_fan_override = -1.;
    }

    this->set_last_pos(path.last_point());
    return gcode;
}

void GCodeGenerator::_add_object_change_labels(std::string& gcode) {
    if (!m_gcode_label_objects_end.empty()) {
        gcode += m_gcode_label_objects_end;
        m_gcode_label_objects_end = "";
        assert(m_gcode_label_objects_in_session);
        m_gcode_label_objects_in_session = false;
    }
    if (!m_gcode_label_objects_start.empty()) {
        gcode += m_gcode_label_objects_start;
        m_gcode_label_objects_start = "";
        assert(!m_gcode_label_objects_in_session);
        m_gcode_label_objects_in_session = true;
    }
}

void GCodeGenerator::ensure_end_object_change_labels(std::string& gcode) {
    if (!m_gcode_label_objects_end.empty()) {
        gcode += m_gcode_label_objects_end;
        m_gcode_label_objects_end = "";
        assert(m_gcode_label_objects_in_session);
        m_gcode_label_objects_in_session = false;
    }
}

// This method accepts &point in print coordinates.
// note: currently, role is only used to check against support by needs_retraction
Polyline GCodeGenerator::travel_to(std::string &gcode, const Point &point, ExtrusionRole role)
{
        /*  Define the travel move as a line between current position and the taget point.
        This is expressed in print coordinates, so it will need to be translated by
        this->origin in order to get G-code coordinates.  */
    Polyline travel { this->last_pos(), point };

    // check whether wipe could be disabled without causing visible stringing
    //not used anymore, not reliable
    bool could_be_wipe_disabled       = false;
    // Save state of use_external_mp_once for the case that will be needed to call twice m_avoid_crossing_perimeters.travel_to.
    const bool used_external_mp_once  = m_avoid_crossing_perimeters.used_external_mp_once();
    const bool used_disabled_once  = m_avoid_crossing_perimeters.disabled_once();

    //can use the avoid crossing algo?
    bool can_avoid_cross_peri = m_config.avoid_crossing_perimeters
        && !m_avoid_crossing_perimeters.disabled_once()
        && m_avoid_crossing_perimeters.is_init()
        && !(m_config.avoid_crossing_not_first_layer && this->on_first_layer());
    
    // check / compute avoid_crossing_perimeters
    bool may_need_avoid_crossing = can_avoid_cross_peri && this->needs_retraction(travel, role, scale_d(EXTRUDER_CONFIG_WITH_DEFAULT(nozzle_diameter, 0.4)) * 3);
    
    if (may_need_avoid_crossing) {
        // if a retraction would be needed (with a low min_dist threshold), try to use avoid_crossing_perimeters to
        // plan a multi-hop travel path inside the configuration space
        if (this->can_cross_perimeter(travel, true)) {
            this->m_throw_if_canceled();
            travel = m_avoid_crossing_perimeters.travel_to(*this, point, &could_be_wipe_disabled);
        }
    }

    // check whether a straight travel move would need retraction
    bool needs_retraction = this->needs_retraction(travel, role);
    if (m_config.only_retract_when_crossing_perimeters &&
        !(m_config.enforce_retract_first_layer && m_layer_index == 0))
        needs_retraction = needs_retraction && this->can_cross_perimeter(travel, true);

    // Re-allow avoid_crossing_perimeters for the next travel moves
    m_avoid_crossing_perimeters.reset_once_modifiers();

    // generate G-code for the travel move
    if (needs_retraction) {
        if (m_config.avoid_crossing_perimeters && EXTRUDER_CONFIG_WITH_DEFAULT(wipe_only_crossing, true)) {
            //if (could_be_wipe_disabled) {
            //    m_wipe.reset_path();
            //} else {
            //check if it cross hull

            //TODO: add bbox cache & checks like for can_cross_perimeter
            bool has_intersect = false;
            for (const ExPolygon &expoly : m_layer->lslices) {
                // first, check if it's inside the contour (still, it can go over holes)
                Polylines diff_result = diff_pl(travel, expoly.contour);
                if (diff_result.size() == 1 && diff_result.front() == travel)
                    // not inside/cross this contour, try another one.
                    continue;
                if (!diff_result.empty()) {
                    //it's crossing this contour!
                    has_intersect = true;
                } else {
                    // it's inside this contour, does it cross a hole?
                    Line  travel_line;
                    Point whatever;
                    for (size_t idx_travel = travel.size() - 1; idx_travel > 0; --idx_travel) {
                        travel_line.a = travel.points[idx_travel];
                        travel_line.b = travel.points[idx_travel - 1];
                        for (const Polygon &hole : expoly.holes) {
                            if (hole.first_intersection(travel_line, &whatever) ||
                                Line(hole.first_point(), hole.last_point()).intersection(travel_line, &whatever)) {
                                has_intersect = true;
                                break;
                            }
                        }
                    }
                }
                break;
            }
            if (!has_intersect) {
                m_wipe.reset_path();
            }
            //}
        }

        Point last_post_before_retract = this->last_pos();

        bool no_lift_on_retract = travel.length() <= scale_(EXTRUDER_CONFIG_WITH_DEFAULT(retract_lift_before_travel, 0));
        gcode += this->retract_and_wipe(false, no_lift_on_retract /*, comment*/);

        // When "Wipe while retracting" is enabled, then extruder moves to another position, and travel from this position can cross perimeters.
        bool updated_first_pos = false;
        if (last_post_before_retract != this->last_pos() && can_avoid_cross_peri) {
            // FIXME Lukas H.: Try to predict if this second calling of avoid crossing perimeters will be needed or not. It could save computations.

            // Is the distance is short enough to just shortcut it?
            if (last_post_before_retract.distance_to(this->last_pos()) > scale_d(EXTRUDER_CONFIG_WITH_DEFAULT(nozzle_diameter, 0.4)) * 2) {

                 // If in the previous call of m_avoid_crossing_perimeters.travel_to was use_external_mp_once set to true restore this value for next call.
                if (used_external_mp_once)
                    m_avoid_crossing_perimeters.use_external_mp_once();
                if (used_disabled_once)
                    m_avoid_crossing_perimeters.disable_once();
                
                this->m_throw_if_canceled();
                // Because of it, it is necessary to redo the thing
                travel = m_avoid_crossing_perimeters.travel_to(*this, point);
                updated_first_pos = true;
                // If state of use_external_mp_once was changed reset it to right value.
                if (used_external_mp_once)
                    m_avoid_crossing_perimeters.reset_once_modifiers();
            }
        }
        if (!updated_first_pos) {
            travel.points.front() = this->last_pos();
        }
    } else {
        // Reset the wipe path when traveling, so one would not wipe along an old path.
        m_wipe.reset_path();
    }
    //if needed, write the gcode_label_objects_end then gcode_label_objects_start
    _add_object_change_labels(gcode);
    
    this->m_throw_if_canceled();
    //if needed, remove points to avoid surcharging the printer.
    {
        const coordf_t scaled_min_length         = scale_d(this->config().min_length.value);
        const double   max_gcode_per_second      = this->config().max_gcode_per_second.value;
        double         current_scaled_min_length = scaled_min_length;
        if (max_gcode_per_second > 0) {
            current_scaled_min_length = std::max(current_scaled_min_length, 
                scale_t(m_config.get_computed_value("travel_speed")) / max_gcode_per_second);
        }
        if (current_scaled_min_length > 0) {
            // it's an alternative to simplifed_path.simplify(scale_(this->config().min_length)); with more enphasis on
            // the segment length that on the feature detail. because tolerance = min_length /10, douglas_peucker will
            // erase more points if angles are shallower than 6° and then the '_plus' will kick in to keep a bit more. if
            // angles are all bigger than 6°, then the douglas_peucker will do all the work.
            // NOTE: okay to use set_points() as i have checked against the absence of arc.
            travel.points = MultiPoint::douglas_peucker_plus(travel.points, current_scaled_min_length / 10, current_scaled_min_length);
        }
    }

    return travel;
}


void GCodeGenerator::write_travel_to(std::string &gcode, const Polyline& travel, std::string comment)
{
    if (travel.size() > 4)
    {
        //ensure that you won't overload the firmware.
        // travel are  strait lines, but with avoid_crossing_perimeters, there can be many points. Reduce speed instead of deleting points, as it's already optimised as much as possible, even if it can be a bit more => TODO?)
        // we are using a window of 10 moves.
        const double max_gcode_per_second = this->config().max_gcode_per_second.value;
        coordf_t dist_next_10_moves = 0;
        size_t idx_10 = 1;
        size_t idx_print = 1;
        const double max_speed = m_config.get_computed_value("travel_speed");
        double current_speed = max_speed;
        for (; idx_10 < travel.size() && idx_10 < 11; ++idx_10) {
            dist_next_10_moves += travel.points[idx_10 - 1].distance_to(travel.points[idx_10]);
        }

        while (idx_10 < travel.size()) {
            //compute speed
            double time_for10_moves = unscaled(dist_next_10_moves) / current_speed;
            double min_time = 10 / max_gcode_per_second;
            double ratio_speed = time_for10_moves / min_time;
            double possible_speed = current_speed * ratio_speed;

            //write
            if (possible_speed < max_speed) {
                if(ratio_speed < 0.95 || ratio_speed > 1.05)
                    current_speed = possible_speed;
            } else if (current_speed < max_speed) {
                current_speed = max_speed;
            }
            gcode += m_writer.travel_to_xy(
                this->point_to_gcode(travel.points[idx_print]),
                current_speed>2 ? double(uint32_t(current_speed)) : current_speed,
                comment);

            //update for next move
            dist_next_10_moves -= travel.points[idx_print - 1].distance_to(travel.points[idx_print]);
            dist_next_10_moves += travel.points[idx_10 - 1].distance_to(travel.points[idx_10]);
            idx_10++;
            idx_print++;
        }

        if (travel.size() < 10) {
            //compute a global speed
            double time_for10_moves = unscaled(dist_next_10_moves) / current_speed;
            double min_time = travel.size() / max_gcode_per_second;
            double ratio_speed = time_for10_moves / min_time;
           current_speed *= ratio_speed;
           current_speed = std::min(max_speed, current_speed);
        } else {
            //compute speed for the end
            double time_for10_moves = unscaled(dist_next_10_moves) / current_speed;
            double min_time = 10 / max_gcode_per_second;
            double ratio_speed = time_for10_moves / min_time;
            current_speed *= ratio_speed;
            current_speed = std::min(max_speed, current_speed);
        }

        //finish writing moves at current speed
        for (; idx_print < travel.size(); ++idx_print)
            gcode += m_writer.travel_to_xy(this->point_to_gcode(travel.points[idx_print]),
                current_speed > 2 ? double(uint32_t(current_speed)) : current_speed,
                comment);
        this->set_last_pos(travel.points.back());
    } else if (travel.size() >= 2) {
        for (size_t i = 1; i < travel.size(); ++i)
            // use G1 because we rely on paths being straight (G0 may make round paths)
            gcode += m_writer.travel_to_xy(this->point_to_gcode(travel.points[i]), 0.0, comment);
        this->set_last_pos(travel.points.back());
    }
}

std::string GCodeGenerator::generate_travel_gcode(
    const Points3& travel,
    const std::string& comment
) {
    std::string gcode;

    const unsigned acceleration =(unsigned)(m_config.travel_acceleration.value + 0.5);

    if (travel.empty()) {
        return "";
    }

    // generate G-code for the travel move
    // use G1 because we rely on paths being straight (G0 may make round paths)
    this->m_writer.set_travel_acceleration(acceleration);

    Vec3d previous_point{this->point_to_gcode(travel.front())};
    for (const Vec3crd& point : travel) {
        const Vec3d gcode_point{this->point_to_gcode(point)};

        assert(previous_point == this->m_writer.get_position());
        gcode += this->m_writer.travel_to_xyz(gcode_point, 0.0, comment);
        this->set_last_pos(point.head<2>());
        previous_point = gcode_point;
    }

    if (! GCodeWriter::supports_separate_travel_acceleration(config().gcode_flavor)) {
        // In case that this flavor does not support separate print and travel acceleration,
        // reset acceleration to default.
        this->m_writer.set_travel_acceleration(acceleration);
    }

    return gcode;
}

Polyline GCodeGenerator::generate_travel_xy_path(
    const Point& start_point,
    const Point& end_point,
    const bool needs_retraction,
    bool& could_be_wipe_disabled
) {

    const Point scaled_origin{scaled(this->origin())};
    const bool avoid_crossing_perimeters = (
        this->m_config.avoid_crossing_perimeters
        && !this->m_avoid_crossing_perimeters.disabled_once()
    );

    Polyline xy_path{start_point, end_point};
    if (m_config.avoid_crossing_curled_overhangs) {
        if (avoid_crossing_perimeters) {
            BOOST_LOG_TRIVIAL(warning)
                << "Option >avoid crossing curled overhangs< is not compatible with avoid crossing perimeters and it will be ignored!";
        } else {
            xy_path = this->m_avoid_crossing_curled_overhangs.find_path(
                start_point + scaled_origin,
                end_point + scaled_origin
            );
            xy_path.translate(-scaled_origin);
        }
    }


    // if a retraction would be needed, try to use avoid_crossing_perimeters to plan a
    // multi-hop travel path inside the configuration space
    if (
        needs_retraction
        && avoid_crossing_perimeters
    ) {
        xy_path = this->m_avoid_crossing_perimeters.travel_to(*this, end_point, &could_be_wipe_disabled);
    }

    return xy_path;
}
/*
// This method accepts &point in print coordinates.
std::string GCodeGenerator::travel_to(
    const Point &start_point, const Point &end_point, ExtrusionRole role, const std::string &comment
) {
    // check whether a straight travel move would need retraction

    bool could_be_wipe_disabled {false};
    bool needs_retraction = this->needs_retraction(Polyline{start_point, end_point}, role);

    Polyline xy_path{generate_travel_xy_path(
        start_point, end_point, needs_retraction, could_be_wipe_disabled
    )};

    needs_retraction = this->needs_retraction(xy_path, role);

    std::string wipe_retract_gcode{};
    if (needs_retraction) {
        if (could_be_wipe_disabled) {
            m_wipe.reset_path();
        }

        Point position_before_wipe{*this->last_position};
        wipe_retract_gcode = this->retract_and_wipe();

        if (*this->last_position != position_before_wipe) {
            xy_path = generate_travel_xy_path(
                *this->last_position, end_point, needs_retraction, could_be_wipe_disabled
            );
        }
    } else {
        m_wipe.reset_path();
    }

    this->m_avoid_crossing_perimeters.reset_once_modifiers();

    const unsigned extruder_id = this->m_writer.extruder()->id();
    const double retract_length = this->m_config.retract_length.get_at(extruder_id);
    bool can_be_flat{!needs_retraction || retract_length == 0};
    const double initial_elevation = this->m_last_layer_z;

    const double upper_limit = this->m_config.retract_lift_below.get_at(extruder_id);
    const double lower_limit = this->m_config.retract_lift_above.get_at(extruder_id);
    if ((lower_limit > 0 && initial_elevation < lower_limit) ||
        (upper_limit > 0 && initial_elevation > upper_limit)) {
        can_be_flat = true;
    }

    const Points3 travel = (
        can_be_flat ?
        GCode::Impl::Travels::generate_flat_travel(xy_path.points, initial_elevation) :
        GCode::Impl::Travels::generate_travel_to_extrusion(
            xy_path,
            m_config,
            extruder_id,
            initial_elevation,
            m_travel_obstacle_tracker,
            scaled(m_origin)
        )
    );

    return wipe_retract_gcode + generate_travel_gcode(travel, comment);
}
*/

bool GCodeGenerator::needs_retraction(const Polyline& travel, ExtrusionRole role /*=ExtrusionRole::None*/, coordf_t max_min_dist /*=0*/)
{
    coordf_t min_dist = scale_d(EXTRUDER_CONFIG_WITH_DEFAULT(retract_before_travel, 0));
    if (max_min_dist > 0)
        min_dist = std::min(max_min_dist, min_dist);
    if (! m_writer.tool() || travel.length() < min_dist) {
        // skip retraction if the move is shorter than the configured threshold
        return false;
    }

    if (role == ExtrusionRole::SupportMaterial) {
        if (const SupportLayer *support_layer = dynamic_cast<const SupportLayer*>(m_layer);
                support_layer != nullptr && ! support_layer->support_islands_bboxes.empty()) {
            BoundingBox bbox_travel = get_extents(travel);
            Polylines   trimmed;
            bool        trimmed_initialized = false;
            for (const BoundingBox &bbox : support_layer->support_islands_bboxes) {
                if (bbox.overlap(bbox_travel)) {
                    const auto &island = support_layer->support_islands[&bbox - support_layer->support_islands_bboxes.data()];
                    trimmed = trimmed_initialized ? diff_pl(trimmed, island) : diff_pl(travel, island);
                    trimmed_initialized = true;
                    if (trimmed.empty())
                        // skip retraction if this is a travel move inside a support material island
                        //FIXME not retracting over a long path may cause oozing, which in turn may result in missing material
                        // at the end of the extrusion path!
                        return false;
                    // Not sure whether updating the boudning box isn't too expensive.
                    //bbox_travel = get_extents(trimmed);
                }
            }
        }
    }

    return true;
}

bool GCodeGenerator::can_cross_perimeter(const Polyline& travel, bool offset)
{
    if (m_layer != nullptr) {
        if (((m_config.only_retract_when_crossing_perimeters &&
              !(m_config.enforce_retract_first_layer && m_layer_index == 0)) &&
             m_config.fill_density.value > 0) ||
            m_config.avoid_crossing_perimeters) {
            if (m_layer_slices_offseted.layer != m_layer) {
                m_layer_slices_offseted.layer    = m_layer;
                m_layer_slices_offseted.diameter = scale_t(EXTRUDER_CONFIG_WITH_DEFAULT(nozzle_diameter, 0.4)) / 2;
                ExPolygons slices                = m_layer->lslices;
                ExPolygons slices_offsetted = offset_ex(m_layer->lslices, -m_layer_slices_offseted.diameter * 1.5f);
                // remove top surfaces
                for (const LayerRegion *reg : m_layer->regions()) {
                    m_throw_if_canceled();
                    slices_offsetted = diff_ex(slices_offsetted, to_expolygons(reg->fill_surfaces().filter_by_type_flag(SurfaceType::stPosTop)));
                    slices           = diff_ex(slices, to_expolygons(reg->fill_surfaces().filter_by_type_flag(SurfaceType::stPosTop)));
                }
                // create bb for speeding things up.
                m_layer_slices_offseted.slices.clear();
                for (ExPolygon &ex : slices) {
                    BoundingBox bb{ex.contour.points};
                    // simplify as much as possible
                    for (ExPolygon &ex_simpl : ex.simplify(m_layer_slices_offseted.diameter)) {
                        m_layer_slices_offseted.slices.emplace_back(std::move(ex_simpl), std::move(bb));
                    }
                }
                m_layer_slices_offseted.slices_offsetted.clear();
                for (ExPolygon &ex : slices_offsetted) {
                    BoundingBox bb{ex.contour.points};
                    for (ExPolygon &ex_simpl : ex.simplify(m_layer_slices_offseted.diameter)) {
                        m_layer_slices_offseted.slices_offsetted.emplace_back(std::move(ex_simpl), std::move(bb));
                    }
                }
            }
        //{
        //    static int aodfjiaqsdz = 0;
        //    std::stringstream stri;
        //    stri << this->m_layer->id() << "_avoid_" <<"_"<<(aodfjiaqsdz++) << ".svg";
        //    SVG svg(stri.str());
        //    svg.draw(m_layer->lslices, "grey");
        //    for (auto &entry : offset ? m_layer_slices_offseted.slices_offsetted : m_layer_slices_offseted.slices) {
        //        bool checked  = (travel.size() > 1 && 
        //            (entry.second.contains(travel.front()) ||
        //            entry.second.contains(travel.back()) ||
        //            entry.second.contains(travel.points[travel.size() / 2]) ||
        //            entry.second.cross(travel) )
        //            );
        //        svg.draw((entry.second.polygon().split_at_first_point()), checked?"green":"orange", scale_t(0.03));
        //        int diff_count =0;
        //        if(checked)
        //            diff_count = diff_pl(travel, entry.first.contour).size();
        //        svg.draw(to_polylines(entry.first), diff_count==0?"blue":diff_count==1?"teal":"yellow", scale_t(0.05));
        //    }
        //    svg.draw(travel, "red", scale_t(0.05));
        //    svg.Close();
        //}
            // test if a expoly contains the entire travel
            for (const std::pair<ExPolygon, BoundingBox> &expoly_2_bb :
                 offset ? m_layer_slices_offseted.slices_offsetted : m_layer_slices_offseted.slices) {
                // first check if it's roughtly inside the bb, to reject quickly.
                auto sec = expoly_2_bb.second;
                if (travel.size() > 1 && 
                    (expoly_2_bb.second.contains(travel.front()) ||
                    expoly_2_bb.second.contains(travel.back()) ||
                    expoly_2_bb.second.contains(travel.points[travel.size() / 2]) ||
                    expoly_2_bb.second.cross(travel) )
                    ) {
                    // first, check if it's inside the contour (still, it can go over holes)
                    Polylines diff_result = diff_pl(travel, expoly_2_bb.first.contour);
                    if (diff_result.size() == 1 && diff_result.front() == travel)
                    //if (!diff_pl(travel, expoly_2_bb.first.contour).empty())
                        continue;
                    //second, check if it's crossing this contour
                    if (!diff_result.empty()) {
                        //has_intersect = true;
                        return true;
                    }
                    // third, check if it's going over a hole
                    // TODO: kdtree to get the ones interesting
                    //bool  has_intersect = false;
                    Line  travel_line;
                    Point whatever;
                    for (const Polygon &hole : expoly_2_bb.first.holes) {
                        m_throw_if_canceled();
                        for (size_t idx_travel = travel.size() - 1; idx_travel > 0; --idx_travel) {
                            travel_line.a = travel.points[idx_travel];
                            travel_line.b = travel.points[idx_travel - 1];
                            if (hole.first_intersection(travel_line, &whatever) ||
                                Line(hole.first_point(), hole.last_point()).intersection(travel_line, &whatever)) {
                                //has_intersect = true;
                                //break;
                                return true;
                            }
                        }
                    }
                    //note: can be inside multiple contours, so we need to checl all of them
                }
            }
            // never crossed a perimeter or a hole
            return false;
        }
    }
    // retract if only_retract_when_crossing_perimeters is disabled or doesn't apply
    return true;
}

std::string GCodeGenerator::retract_and_wipe(bool toolchange, bool inhibit_lift)
{
    std::string gcode;

    if (m_writer.tool() == nullptr)
        return gcode;

    // We need to reset e before any extrusion or wipe to allow the reset to happen at the real 
    // begining of an object gcode
    gcode += m_writer.reset_e();
    
    // wipe (if it's enabled for this extruder and we have a stored wipe path)
    if (BOOL_EXTRUDER_CONFIG(wipe) && m_wipe.has_path()) {
        gcode += toolchange ? m_writer.retract_for_toolchange(true) : m_writer.retract(true);
        gcode += m_wipe.wipe(*this, toolchange);
    }

    /*  The parent class will decide whether we need to perform an actual retraction
        (the extruder might be already retracted fully or partially). We call these
        methods even if we performed wipe, since this will ensure the entire retraction
        length is honored in case wipe path was too short.  */
    gcode += toolchange ? m_writer.retract_for_toolchange() : m_writer.retract();

    if (!inhibit_lift) {
        // check if need to lift
        bool need_lift = !m_writer.tool_is_extruder() || toolchange
            || (BOOL_EXTRUDER_CONFIG(retract_lift_first_layer) && m_config.print_retract_lift.value != 0 && this->m_layer_index == 0)
            || this->m_writer.get_extra_lift() > 0;
        bool last_fill_extusion_role_top_infill = (this->m_last_extrusion_role == GCodeExtrusionRole::TopSolidInfill || this->m_last_extrusion_role == GCodeExtrusionRole::Ironing);
        if (this->m_last_extrusion_role == GCodeExtrusionRole::GapFill)
            last_fill_extusion_role_top_infill = (this->m_last_notgapfill_extrusion_role == GCodeExtrusionRole::TopSolidInfill || this->m_last_notgapfill_extrusion_role == GCodeExtrusionRole::Ironing);
        if (!need_lift && m_config.print_retract_lift.value != 0) {
            if (EXTRUDER_CONFIG_WITH_DEFAULT(retract_lift_top, "") == "Not on top")
                need_lift = !last_fill_extusion_role_top_infill;
            else if (EXTRUDER_CONFIG_WITH_DEFAULT(retract_lift_top, "") == "Only on top")
                need_lift = last_fill_extusion_role_top_infill;
            else
                need_lift = true;
        }
        if (need_lift)
            gcode += m_writer.lift(this->m_layer_index);
    }
    return gcode;
}

std::string GCodeGenerator::toolchange(uint16_t extruder_id, double print_z) {

    std::string gcode;
    // Process the custom toolchange_gcode. If it is empty, insert just a Tn command.
    std::string toolchange_gcode = m_config.toolchange_gcode.value;
    boost::trim(toolchange_gcode); //remove invisible characters that may compromize the 'toolchange_gcode.empty()'
    std::string toolchange_gcode_parsed;
    if (!toolchange_gcode.empty() && m_writer.multiple_extruders) {
        DynamicConfig config;
        config.set_key_value("previous_extruder", new ConfigOptionInt((int)(m_writer.tool() != nullptr ? m_writer.tool()->id() : -1)));
        config.set_key_value("next_extruder", new ConfigOptionInt((int)extruder_id));
        config.set_key_value("layer_num", new ConfigOptionInt(m_layer_index));
        config.set_key_value("layer_z", new ConfigOptionFloat(print_z));
        config.set_key_value("toolchange_z", new ConfigOptionFloat(print_z));
        config.set_key_value("max_layer_z", new ConfigOptionFloat(m_max_layer_z));
        toolchange_gcode_parsed = placeholder_parser_process("toolchange_gcode", toolchange_gcode, extruder_id, &config);
        gcode += toolchange_gcode_parsed;
        check_add_eol(gcode);
    }

    // We inform the writer about what is happening, but we may not use the resulting gcode.
    std::string toolchange_command = m_writer.toolchange(extruder_id);
    if (toolchange_gcode.empty() && m_writer.multiple_extruders) { // !custom_gcode_changes_tool(toolchange_gcode_parsed, m_writer.toolchange_prefix(), extruder_id) && !no_toolchange)
        gcode += toolchange_command;
    } else {
        // user provided his own toolchange gcode, no need to do anything
    }
    if (m_enable_cooling_markers) {
        gcode += ";_TOOLCHANGE " + std::to_string(extruder_id) + "\n";
    }
    return gcode;
}

std::string GCodeGenerator::set_extruder(uint16_t extruder_id, double print_z, bool no_toolchange /*=false*/)
{
    if (!m_writer.need_toolchange(extruder_id))
        return "";
    
    //just for testing
    assert(this->writer().get_position().z() - m_config.z_offset.value == print_z);

    // if we are running a single-extruder setup, just set the extruder and return nothing
    if (!m_writer.multiple_extruders) {
        this->placeholder_parser().set("current_extruder", extruder_id);

        std::string gcode;
        // Append the filament start G-code.
        const std::string &start_filament_gcode = m_config.start_filament_gcode.get_at(extruder_id);
        if (! start_filament_gcode.empty()) {
            DynamicConfig config;
            config.set_key_value("layer_num", new ConfigOptionInt(m_layer_index));
            config.set_key_value("layer_z", new ConfigOptionFloat(print_z));
            config.set_key_value("max_layer_z", new ConfigOptionFloat(m_max_layer_z));
            config.set_key_value("filament_extruder_id", new ConfigOptionInt(int(extruder_id)));
            config.set_key_value("previous_extruder", new ConfigOptionInt((int)extruder_id));
            config.set_key_value("next_extruder", new ConfigOptionInt((int)extruder_id));
            // Process the start_filament_gcode for the new filament.
            gcode += this->placeholder_parser_process("start_filament_gcode", start_filament_gcode, extruder_id, &config);
            check_add_eol(gcode);
        }
        if (!no_toolchange) {
            gcode += toolchange(extruder_id, print_z);
        }else m_writer.toolchange(extruder_id);
        return gcode;
    }
    
    // get current extruder id
    uint16_t old_extruder_id = uint16_t(m_writer.tool() != nullptr ? m_writer.tool()->id() : -1);

    // prepend retraction on the current extruder
    std::string gcode = this->retract_and_wipe(true);

    // Always reset the extrusion path, even if the tool change retract is set to zero.
    m_wipe.reset_path();

    if (m_writer.tool() != nullptr) {
        // Process the custom end_filament_gcode. set_extruder() is only called if there is no wipe tower
        // so it should not be injected twice.
        const std::string  &end_filament_gcode  = m_config.end_filament_gcode.get_at(old_extruder_id);
        if (! end_filament_gcode.empty()) {
            DynamicConfig config;
            config.set_key_value("layer_num", new ConfigOptionInt(m_layer_index));
            config.set_key_value("layer_z", new ConfigOptionFloat(print_z));
            config.set_key_value("max_layer_z", new ConfigOptionFloat(m_max_layer_z));
            config.set_key_value("filament_extruder_id", new ConfigOptionInt(int(old_extruder_id)));
            config.set_key_value("previous_extruder", new ConfigOptionInt((int)old_extruder_id));
            config.set_key_value("next_extruder", new ConfigOptionInt((int)extruder_id));
            gcode += placeholder_parser_process("end_filament_gcode", end_filament_gcode, old_extruder_id >= 0 ? old_extruder_id : extruder_id, &config);
            check_add_eol(gcode);
        }
    }


    // If ooze prevention is enabled, park current extruder in the nearest
    // standby point and set it to the standby temperature.
    if (m_ooze_prevention.enable && m_writer.tool() != nullptr)
        gcode += m_ooze_prevention.pre_toolchange(*this);
    

    if (!no_toolchange) {
        gcode += toolchange(extruder_id, print_z);
    }else m_writer.toolchange(extruder_id);

    // Set the temperature if the wipe tower didn't (not needed for non-single extruder MM)
    // supermerill change: try to set the good temp, because the wipe tower don't use the gcode writer and so can write wrong stuff.
    if (m_config.single_extruder_multi_material /*&& !m_config.wipe_tower*/) {
        int temp = (m_layer_index <= 0 && m_config.first_layer_temperature.get_at(extruder_id) > 0 ? m_config.first_layer_temperature.get_at(extruder_id) :
                                         m_config.temperature.get_at(extruder_id));
        if (temp > 0)
            gcode += m_writer.set_temperature(temp, false);
    }

    this->placeholder_parser().set("current_extruder", extruder_id);

    // Append the filament start G-code.
    const std::string &start_filament_gcode = m_config.start_filament_gcode.get_at(extruder_id);
    if (! start_filament_gcode.empty()) {
        DynamicConfig config;
        config.set_key_value("layer_num", new ConfigOptionInt(m_layer_index));
        config.set_key_value("layer_z", new ConfigOptionFloat(print_z));
        config.set_key_value("max_layer_z", new ConfigOptionFloat(m_max_layer_z));
        config.set_key_value("filament_extruder_id", new ConfigOptionInt(int(extruder_id)));
        config.set_key_value("previous_extruder", new ConfigOptionInt((int)old_extruder_id));
        config.set_key_value("next_extruder", new ConfigOptionInt((int)extruder_id));
        // Process the start_filament_gcode for the new filament.
        gcode += this->placeholder_parser_process("start_filament_gcode", start_filament_gcode, extruder_id, &config);
        check_add_eol(gcode);
        //check if it changed the temp
        int  temp_by_gcode = -1;
        if (custom_gcode_sets_temperature(gcode, 104, 109, false, temp_by_gcode)) {
            //set writer
            m_writer.set_temperature(temp_by_gcode, false, extruder_id);
        }
    }
    // Set the new extruder to the operating temperature.
    if (m_ooze_prevention.enable)
        gcode += m_ooze_prevention.post_toolchange(*this);

    // The position is now known after the tool change.
    this->unset_last_pos();
    
    return gcode;
}

// convert a model-space scaled point into G-code coordinates
Vec3d GCodeGenerator::point_to_gcode(const Point &point, const coord_t z_offset) const {
    Vec2d extruder_offset = m_writer.current_tool_offset();
    Vec3d ret_vec(unscaled(point.x()) + m_origin.x() - extruder_offset.x(),
        unscaled(point.y()) + m_origin.y() - extruder_offset.y(),
        unscaled(z_offset));
    return ret_vec;
}

// convert a model-space scaled point into G-code coordinates
Point GCodeGenerator::gcode_to_point(const Vec2d &point) const
{
    Vec2d extruder_offset = m_writer.current_tool_offset(); //EXTRUDER_CONFIG_WITH_DEFAULT(extruder_offset, Vec2d(0, 0)); // FIXME : mill ofsset
    return Point(
        scale_t(point(0) - m_origin(0) + extruder_offset(0)),
        scale_t(point(1) - m_origin(1) + extruder_offset(1)));
}

}   // namespace Slic3r
