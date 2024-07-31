///|/ Copyright (c) Prusa Research 2016 - 2023 Vojtěch Bubník @bubnikv, Lukáš Matěna @lukasmatena, Pavel Mikuš @Godrak, Lukáš Hejl @hejllukas, Filip Sykala @Jony01, Enrico Turri @enricoturri1966, David Kocík @kocikdav, Oleksandra Iushchenko @YuSanka
///|/ Copyright (c) SuperSlicer 2023 Remi Durand @supermerill
///|/ Copyright (c) 2019 Thomas Moore
///|/ Copyright (c) 2016 Chow Loong Jin @hyperair
///|/ Copyright (c) Slic3r 2014 - 2015 Alessandro Ranellucci @alranel
///|/
///|/ ported from lib/Slic3r/GCode.pm:
///|/ Copyright (c) Slic3r 2011 - 2015 Alessandro Ranellucci @alranel
///|/ Copyright (c) 2013 Robert Giseburt
///|/ Copyright (c) 2012 Mark Hindess
///|/ Copyright (c) 2012 Henrik Brix Andersen @henrikbrixandersen
///|/
///|/ PrusaSlicer is released under the terms of the AGPLv3 or higher
///|/
#ifndef slic3r_GCode_hpp_
#define slic3r_GCode_hpp_

#include "GCode/ExtrusionProcessor.hpp"
#include "JumpPointSearch.hpp"
#include "libslic3r.h"
#include "EdgeGrid.hpp"
#include "ExPolygon.hpp"
#include "Layer.hpp"
#include "Point.hpp"
#include "Print.hpp"
#include "PlaceholderParser.hpp"
#include "PrintConfig.hpp"
#include "Geometry/ArcWelder.hpp"
#include "GCode/AvoidCrossingPerimeters.hpp"
#include "GCode/CoolingBuffer.hpp"
#include "GCode/FanMover.hpp"
#include "GCode/FindReplace.hpp"
#include "GCode/GCodeWriter.hpp"
#include "GCode/LabelObjects.hpp"
#include "GCode/PressureEqualizer.hpp"
#include "GCode/RetractWhenCrossingPerimeters.hpp"
// #include "GCode/SmoothPath.hpp"
#include "GCode/SpiralVase.hpp"
#include "GCode/ToolOrdering.hpp"
#include "GCode/Wipe.hpp"
#include "GCode/WipeTowerIntegration.hpp"
#include "GCode/SeamPlacer.hpp"
#include "GCode/GCodeProcessor.hpp"
#include "GCode/ThumbnailData.hpp"
#include "GCode/Travels.hpp"
#include "EdgeGrid.hpp"
#include "tcbspan/span.hpp"

#include <memory>
#include <map>
#include <string>
#include <chrono>

//#include "GCode/PressureEqualizer.hpp"

namespace Slic3r {

// Forward declarations.
class GCodeGenerator;
struct WipeTowerData;

namespace { struct Item; }
struct PrintInstance;

class OozePrevention {
public:
    bool enable;
    
    OozePrevention() : enable(false) {}
    std::string pre_toolchange(GCodeGenerator &gcodegen);
    std::string post_toolchange(GCodeGenerator &gcodegen);
    
private:
    int _get_temp(const GCodeGenerator &gcodegen) const;
};

class ColorPrintColors
{
    static const std::vector<std::string> Colors;
public:
    static const std::vector<std::string>& get() { return Colors; }
};

struct LayerResult {
    std::string gcode;
    size_t      layer_id;
    // Is spiral vase post processing enabled for this layer?
    bool        spiral_vase_enable { false };
    // Should the cooling buffer content be flushed at the end of this layer?
    bool        cooling_buffer_flush { false };
    // Is indicating if this LayerResult should be processed, or it is just inserted artificial LayerResult.
    // It is used for the pressure equalizer because it needs to buffer one layer back.
    bool        nop_layer_result { false };

    static LayerResult make_nop_layer_result() { return {"", std::numeric_limits<coord_t>::max(), false, false, true}; }
};

namespace GCode {
// Object and support extrusions of the same PrintObject at the same print_z.
// public, so that it could be accessed by free helper functions from GCode.cpp
struct ObjectLayerToPrint
{
    ObjectLayerToPrint() : object_layer(nullptr), support_layer(nullptr) {}
    const Layer* 		object_layer;
    const SupportLayer* support_layer;
    const Layer* 		layer()   const { return (object_layer != nullptr) ? object_layer : support_layer; }
    const PrintObject* 	object()  const { return (this->layer() != nullptr) ? this->layer()->object() : nullptr; }
    coordf_t            print_z() const { return (object_layer != nullptr && support_layer != nullptr) ? 0.5 * (object_layer->print_z + support_layer->print_z) : this->layer()->print_z; }
};

struct PrintObjectInstance
{
    const PrintObject *print_object = nullptr;
    int                instance_idx = -1;

    bool operator==(const PrintObjectInstance &other) const {return print_object == other.print_object && instance_idx == other.instance_idx; }
    bool operator!=(const PrintObjectInstance &other) const { return *this == other; }
};

} // namespace GCode

class GCodeGenerator : ExtrusionVisitorConst {

public:
    GCodeGenerator();
    ~GCodeGenerator() = default;

    // throws std::runtime_exception on error,
    // throws CanceledException through print->throw_if_canceled().
    void            do_export(Print* print, const char* path, GCodeProcessorResult* result = nullptr, ThumbnailsGeneratorCallback thumbnail_cb = nullptr);

    // Exported for the helper classes (OozePrevention, Wipe) and for the Perl binding for unit tests.
    const Vec2d&    origin() const { return m_origin; }
    void            set_origin(const Vec2d &pointf);
    void            set_origin(const coordf_t x, const coordf_t y) { this->set_origin(Vec2d(x, y)); }
    const Point&    last_pos() const { assert(m_last_pos); return *m_last_pos; }
    bool            last_pos_defined() const { return m_last_pos.has_value(); }
    void            set_last_pos(const Point &pos) { m_last_pos = pos; }
    void            unset_last_pos() { m_last_pos.reset(); }
    // Convert coordinates of the active object to G-code coordinates, possibly adjusted for extruder offset.
    template<typename Derived>
    Eigen::Matrix<double, Derived::SizeAtCompileTime, 1, Eigen::DontAlign> point_to_gcode(const Eigen::MatrixBase<Derived> &point) const {
        static_assert(
            Derived::IsVectorAtCompileTime,
            "GCodeGenerator::point_to_gcode(): first parameter is not a vector"
        );
        static_assert(
            int(Derived::SizeAtCompileTime) == 2 || int(Derived::SizeAtCompileTime) == 3,
            "GCodeGenerator::point_to_gcode(): first parameter is not a 2D or 3D vector"
        );

        if constexpr (Derived::SizeAtCompileTime == 2) {
            return Vec2d(unscaled<double>(point.x()), unscaled<double>(point.y())) + m_origin
                - m_writer.current_tool_offset();
        } else {
            const Vec2d gcode_point_xy{this->point_to_gcode(point.template head<2>())};
            return to_3d(gcode_point_xy, unscaled(point.z()));
        }
    }

    // Convert coordinates of the active object to G-code coordinates, possibly adjusted for extruder offset and quantized to G-code resolution.
    template<typename Derived>
    Vec2d           point_to_gcode_quantized(const Eigen::MatrixBase<Derived> &point) const {
        static_assert(Derived::IsVectorAtCompileTime && int(Derived::SizeAtCompileTime) == 2, "GCodeGenerator::point_to_gcode_quantized(): first parameter is not a 2D vector");
        Vec2d p = this->point_to_gcode(point);
        return m_writer.get_default_gcode_formatter().quantize(p);
    }
    Vec3d           point_to_gcode(const Point &point, coord_t z_offset) const;
    Point           gcode_to_point(const Vec2d &point) const;
    const FullPrintConfig &config() const { return m_config; }
    const Layer*    layer() const { return m_layer; }
    GCodeWriter&    writer() { return m_writer; }
    const GCodeWriter& writer() const { return m_writer; }
    PlaceholderParser& placeholder_parser() { return m_placeholder_parser_integration.parser; }
    const PlaceholderParser& placeholder_parser() const { return m_placeholder_parser_integration.parser; }
    // Process a template through the placeholder parser, collect error messages to be reported
    // inside the generated string and after the G-code export finishes.
    std::string placeholder_parser_process(const std::string &name,
                                           const std::string &templ,
                                           uint16_t current_extruder_id,
                                           const DynamicConfig *config_override = nullptr);
    bool            enable_cooling_markers() const { return m_enable_cooling_markers; }

    // For Perl bindings, to be used exclusively by unit tests.
    unsigned int    layer_count() const { return m_layer_with_support_count; }
    unsigned int    object_layer_count() const { return m_layer_count; }
    //void            set_layer_count(unsigned int value) { m_layer_count = value; }
    void            apply_print_configs(const Print &print);

    // append full config to the given string
    static void append_full_config(const Print& print, std::string& str);
    // translate full config into a list of <key, value> items
    static void encode_full_config(const Print& print, std::vector<std::pair<std::string, std::string>>& config);

    using ObjectLayerToPrint  = GCode::ObjectLayerToPrint;
    using ObjectsLayerToPrint = std::vector<GCode::ObjectLayerToPrint>;

private:
    class GCodeOutputStream {
    public:
        GCodeOutputStream(FILE* f, GCodeProcessor& processor) : f(f), m_processor(processor) {}
        ~GCodeOutputStream() { this->close(); }

        // Set a find-replace post-processor to modify the G-code before GCodePostProcessor.
        // It is being set to null inside process_layers(), because the find-replace process
        // is being called on a secondary thread to improve performance.
        void set_find_replace(GCodeFindReplace *find_replace, bool enabled) { m_find_replace_backup = find_replace; m_find_replace = enabled ? find_replace : nullptr; }
        void set_only_ascii(bool only_ascii) { m_only_ascii = only_ascii; }
        void find_replace_enable() { m_find_replace = m_find_replace_backup; }
        void find_replace_supress() { m_find_replace = nullptr; }

        bool is_open() const { return f; }
        bool is_error() const;
        
        void flush();
        void close();

        // Write a string into a file.
        void write(const std::string& what) { this->write(what.c_str()); }
        void write(const char* what);

        // Write a string into a file. 
        // Add a newline, if the string does not end with a newline already.
        // Used to export a custom G-code section processed by the PlaceholderParser.
        void writeln(const std::string& what);

        // Formats and write into a file the given data. 
        void write_format(const char* format, ...);

    private:
        FILE             *f { nullptr };
        // Find-replace post-processor to be called before GCodePostProcessor.
        GCodeFindReplace *m_find_replace { nullptr };
        bool              m_only_ascii;
        // If suppressed, the backoup holds m_find_replace.
        GCodeFindReplace *m_find_replace_backup { nullptr };
        GCodeProcessor   &m_processor;
    };
    void            _do_export(Print &print, GCodeOutputStream &file, ThumbnailsGeneratorCallback thumbnail_cb);
    void            _move_to_print_object(std::string& gcode_out, const Print& print, size_t finished_objects, uint16_t initial_extruder_id);
    void            _init_multiextruders(const Print& print, std::string& gcode_out, GCodeWriter& writer, const ToolOrdering& tool_ordering, const std::string& custom_gcode);

    static ObjectsLayerToPrint         		                     collect_layers_to_print(const PrintObject &object, Print::StatusMonitor &status_monitor);
    static std::vector<std::pair<coordf_t, ObjectsLayerToPrint>> collect_layers_to_print(const Print &print, Print::StatusMonitor &status_monitor);

    /** @brief Generates ramping travel gcode for layer change. */
    std::string get_layer_change_gcode(const Vec3d& from, const Vec3d& to, const unsigned extruder_id);

    LayerResult process_layer(
        const Print                     &print,
        Print::StatusMonitor            &status_monitor,
        // Set of object & print layers of the same PrintObject and with the same print_z.
        const ObjectsLayerToPrint       &layers,
        const LayerTools  				&layer_tools,
        const bool                       last_layer,
		// Pairs of PrintObject index and its instance index.
		const std::vector<const PrintInstance*> *ordering,
        // If set to size_t(-1), then print all copies of all objects.
        // Otherwise print a single copy of a single object.
        size_t                           single_object_idx = size_t(-1)
        );
    // Process all layers of all objects (non-sequential mode) with a parallel pipeline:
    // Generate G-code, run the filters (vase mode, cooling buffer), run the G-code analyser
    // and export G-code into file.
    void process_layers(
        const Print                                                   &print,
        Print::StatusMonitor                                          &status_monitor,
        const ToolOrdering                                            &tool_ordering,
        const std::vector<const PrintInstance*>                       &print_object_instances_ordering,
        const std::vector<std::pair<coordf_t, ObjectsLayerToPrint>>   &layers_to_print,
        std::string                                                   &preamble,
        GCodeOutputStream                                             &output_stream);
    // Process all layers of a single object instance (sequential mode) with a parallel pipeline:
    // Generate G-code, run the filters (vase mode, cooling buffer), run the G-code analyser
    // and export G-code into file.
    void process_layers(
        const Print                             &print,
        Print::StatusMonitor                    &status_monitor,
        const ToolOrdering                      &tool_ordering,
        ObjectsLayerToPrint                      layers_to_print,
        const size_t                             single_object_idx,
        std::string                             &preamble,
        GCodeOutputStream                       &output_stream);
    
    void            set_extruders(const std::vector<uint16_t> &extruder_ids);
    std::string     preamble();
    std::string change_layer(
        double previous_layer_z,
        double print_z,
        bool vase_mode
    );

    std::string      visitor_gcode;
    bool             visitor_flipped; //TODO use instead of reverse() at extrude_entity
    bool             visitor_in_use = false;
    std::string_view visitor_comment;
    double           visitor_speed;
    virtual void use(const ExtrusionPath &path) override { visitor_gcode += extrude_path(path, visitor_comment, visitor_speed); };
    virtual void use(const ExtrusionPath3D &path3D) override { visitor_gcode += extrude_path_3D(path3D, visitor_comment, visitor_speed); };
    virtual void use(const ExtrusionMultiPath &multipath) override { visitor_gcode += extrude_multi_path(multipath, visitor_comment, visitor_speed); };
    virtual void use(const ExtrusionMultiPath3D &multipath) override { visitor_gcode += extrude_multi_path3D(multipath, visitor_comment, visitor_speed); };
    virtual void use(const ExtrusionLoop &loop) override { visitor_gcode += extrude_loop(loop, visitor_comment, visitor_speed); };
    virtual void use(const ExtrusionEntityCollection &collection) override;
    std::string     extrude_entity(const ExtrusionEntityReference &entity, const std::string_view description, double speed = -1.);
    std::string     extrude_loop(const ExtrusionLoop &loop, const std::string_view description, double speed = -1.);
    std::string     extrude_loop_vase(const ExtrusionLoop &loop, const std::string_view description, double speed = -1.);
    std::string     extrude_multi_path(const ExtrusionMultiPath &multipath, const std::string_view description, double speed = -1.);
    std::string     extrude_multi_path3D(const ExtrusionMultiPath3D &multipath, const std::string_view description, double speed = -1.);
    std::string     extrude_path(const ExtrusionPath &path, const std::string_view description, double speed = -1.);
    std::string     extrude_path_3D(const ExtrusionPath3D &path, const std::string_view description, double speed = -1.);

    void            split_at_seam_pos(ExtrusionLoop &loop, bool was_clockwise);
    template <typename THING = ExtrusionEntity> // can be templated safely because private
    void            add_wipe_points(const std::vector<THING>& paths, bool reverse = true);
    void            seam_notch(const ExtrusionLoop& original_loop, ExtrusionPaths& building_paths,
        ExtrusionPaths& notch_extrusion_start, ExtrusionPaths& notch_extrusion_end, bool is_hole_loop, bool is_full_loop_ccw);


	struct InstanceToPrint
	{
        InstanceToPrint(size_t object_layer_to_print_id, const PrintObject &print_object, size_t instance_id) :
            object_layer_to_print_id(object_layer_to_print_id), print_object(print_object), instance_id(instance_id) {}

        // Index into std::vector<ObjectLayerToPrint>, which contains Object and Support layers for the current print_z, collected for a single object, or for possibly multiple objects with multiple instances.
        const size_t             object_layer_to_print_id;
		const PrintObject 		&print_object;
		// Instance idx of the copy of a print object.
		const size_t			 instance_id;
	};

	std::vector<InstanceToPrint> sort_print_object_instances(
		// Object and Support layers for the current print_z, collected for a single object, or for possibly multiple objects with multiple instances.
        const std::vector<ObjectLayerToPrint>           &layers,
		// Ordering must be defined for normal (non-sequential print).
		const std::vector<const PrintInstance*>     	*ordering,
		// For sequential print, the instance of the object to be printing has to be defined.
		const size_t                     				 single_object_instance_idx);

    struct ExtrudeArgs{
        // Index of the extruder currently active.
        uint16_t                  extruder_id;
        // What object and instance is going to be printed.
        const InstanceToPrint    &print_instance;
        // Container for extruder overrides (when wiping into object or infill).
        const LayerTools         &layer_tools;
        // Is any extrusion possibly marked as wiping extrusion?
        bool                      is_anything_overridden;
        // Round 1 (wiping into object or infill) or round 2 (normal extrusions).
        bool                      print_wipe_extrusions;
    };
    
    // This function will be called for each printing extruder, possibly twice: First for wiping extrusions, second for normal extrusions.
    void process_layer_single_object(
        // output
        std::string              &gcode, 
        const ExtrudeArgs        &args,
        // and the object & support layer of the above.
        const ObjectLayerToPrint &layer_to_print);
    void emit_milling_commands(std::string& gcode, const ObjectsLayerToPrint& layers);
    
    // set the region config, and the overrides it contains.
    // if no m_region, then it will take the default region config from print_object
    // if no print_object, then it will take the default region config from print
    void set_region_for_extrude(const Print &print, const PrintObject *print_object, std::string &gcode);
    void extrude_perimeters(const ExtrudeArgs &print_args, const LayerIsland &island, std::string &gcode);
    void extrude_infill(const ExtrudeArgs &print_args, const LayerIsland &island, bool is_infill_first, std::string &gcode);
    void extrude_ironing(const ExtrudeArgs &print_args, const LayerIsland &island, std::string &gcode);
    void extrude_skirt(ExtrusionLoop &loop_src, const ExtrusionFlow &extrusion_flow_override, std::string &gcode, const std::string_view description);
    std::string     extrude_support(const ExtrusionEntityReferences &support_fills);
    bool            shall_print_this_extrusion_collection(const ExtrudeArgs &              print_args,
                                                          const ExtrusionEntityCollection *eec,
                                                          const PrintRegion &              region);
    // for helical layer change for vase mode
     std::string generate_travel_gcode(
         const Points3& travel,
         const std::string& comment
     );
    // for helical layer change for vase mode
     Polyline generate_travel_xy_path(
         const Point& start,
         const Point& end,
         const bool needs_retraction,
         bool& could_be_wipe_disabled
     );
    Polyline        travel_to(std::string& gcode, const Point &end_point, ExtrusionRole role);
    void            write_travel_to(std::string& gcode, const Polyline& travel, std::string comment);
    std::string     travel_to_first_position(const Vec3crd& point);
    bool            can_cross_perimeter(const Polyline& travel, bool offset);
    bool            needs_retraction(const Polyline &travel, ExtrusionRole role = ExtrusionRole::None, coordf_t max_min_dist = 0);

    std::string     retract_and_wipe(bool toolchange = false, bool inhibit_lift = false);
    std::string     unretract() { return m_writer.unlift() + m_writer.unretract(); }
    std::string     set_extruder(uint16_t extruder_id, double print_z, bool no_toolchange = false);
    std::string     toolchange(uint16_t extruder_id, double print_z);
    bool line_distancer_is_required(const std::vector<uint16_t>& extruder_ids);

    // Cache for custom seam enforcers/blockers for each layer.
    SeamPlacer                          m_seam_placer;
    bool                                m_seam_perimeters = false;
    public:
    /* Origin of print coordinates expressed in unscaled G-code coordinates.
       This affects the input arguments supplied to the extrude*() and travel_to()
       methods. */
    Vec2d                               m_origin;
    FullPrintConfig                     m_config;
    GCodeWriter                         m_writer;

    struct PlaceholderParserIntegration {
        void reset();
        void init(const PrintConfig &print_config, const GCodeWriter &config);
        void update_from_gcodewriter(const GCodeWriter &writer, const WipeTowerData& wipe_tower_data);
        void validate_output_vector_variables();

        PlaceholderParser                   parser;
    // For random number generator etc.
        PlaceholderParser::ContextData      context;
    // Collection of templates, on which the placeholder substitution failed.
        std::map<std::string, std::string>  failed_templates;
        // Input/output from/to custom G-code block, for returning position, retraction etc.
        // output_config contains unique_ptr of ConfigOptions
        DynamicConfig                       output_config;
        // these are pointer to unique_ptr from output_config
        ConfigOptionFloats                 *opt_e_retracted { nullptr };
        ConfigOptionFloats                 *opt_e_restart_extra { nullptr };
        ConfigOptionFloats                 *opt_e_position { nullptr };
        ConfigOptionFloats                 *opt_position { nullptr };
        // these are pointer to unique_ptr from parser.m_config
        ConfigOptionFloats                 *opt_position_parser { nullptr };
        ConfigOptionFloat                  *opt_zhop { nullptr };
        ConfigOptionFloats                 *opt_extruded_volume { nullptr };
        ConfigOptionFloats                 *opt_extruded_weight { nullptr };
        ConfigOptionFloat                  *opt_extruded_volume_total { nullptr };
        ConfigOptionFloat                  *opt_extruded_weight_total { nullptr };
        ConfigOptionInts                   *opt_extruder_colour_int { nullptr };
        ConfigOptionInts                   *opt_filament_colour_int { nullptr };
        // Caches of the data passed to the script.
        size_t                              num_extruders;
        std::vector<double>                 position;
        std::vector<double>                 e_position;
        std::vector<double>                 e_retracted;
        std::vector<double>                 e_restart_extra;
    } m_placeholder_parser_integration;

    OozePrevention                      m_ooze_prevention;
    GCode::Wipe                         m_wipe;
    GCode::LabelObjects                 m_label_objects;
    AvoidCrossingPerimeters             m_avoid_crossing_perimeters;
    JPSPathFinder                       m_avoid_crossing_curled_overhangs;
    RetractWhenCrossingPerimeters       m_retract_when_crossing_perimeters;
    GCode::TravelObstacleTracker        m_travel_obstacle_tracker;
    bool                                m_enable_loop_clipping;
    // If enabled, the G-code generator will put following comments at the ends
    // of the G-code lines: _EXTRUDE_SET_SPEED, _WIPE, _BRIDGE_FAN_START, _BRIDGE_FAN_END, _BRIDGE_INTERNAL_FAN_START, _BRIDGE_INTERNAL_FAN_END
    // Those comments are received and consumed (removed from the G-code) by the CoolingBuffer.pm Perl module.
    bool                                m_enable_cooling_markers;
    // Markers for the Pressure Equalizer to recognize the extrusion type.
    // The Pressure Equalizer removes the markers from the final G-code.
    bool                                m_enable_extrusion_role_markers;
    // HACK to avoid multiple Z move.
    std::string                         m_delayed_layer_change;
    // Keeps track of the last extrusion role passed to the processor
    GCodeExtrusionRole                  m_last_processor_extrusion_role;
    // For Progress bar indicator, in sequential mode (complete objects)
    std::set<const PrintObject*>              m_object_sequentially_printed;
    // How many times will change_layer() be called?
    // change_layer() will update the progress bar.
    uint32_t                            m_layer_count;
    uint32_t                            m_layer_with_support_count;
    // Progress bar indicator. Increments from -1 up to layer_count.
    int                                 m_layer_index;
    // Current layer processed. In sequential printing mode, only a single copy will be printed.
    // In non-sequential mode, all its copies will be printed.
    const Layer*                        m_layer;
    const PrintRegion*                  m_region = nullptr;
    // m_layer is an object layer and it is being printed over raft surface.
    bool                                m_object_layer_over_raft;    // idx of the current instance printed. (or the last one)
    uint16_t                            m_print_object_instance_id = -1;
    // For crossing perimeter retraction detection  (contain the layer & nozzle widdth used to construct it)
    // !!!! not thread-safe !!!! if threaded per layer, please store it in the thread.
    struct SliceOffsetted {
        std::vector<std::pair<ExPolygon, BoundingBox>> slices;
        std::vector<std::pair<ExPolygon, BoundingBox>> slices_offsetted;
        const Layer* layer;
        coord_t diameter;
    }                                   m_layer_slices_offseted{ {},{},nullptr, 0};
    double                              m_volumetric_speed;
    // Support for the extrusion role markers. Which marker is active?
    GCodeExtrusionRole                  m_last_extrusion_role;
    // Not know the gapfill role for retract_lift_top
    GCodeExtrusionRole                  m_last_notgapfill_extrusion_role;
    // Support for G-Code Processor
    double                              m_last_height{ 0.0 };
    double                              m_last_layer_z{ 0.0 };
    double                              m_max_layer_z{ 0.0 };
    double                              m_last_width{ 0.0 };
    // to pass between before_xtrude and after_extrude.
    double                              m_last_fan_override{ -1.0 };
#if ENABLE_GCODE_VIEWER_DATA_CHECKING
    double                              m_last_mm3_per_mm;
#endif // ENABLE_GCODE_VIEWER_DATA_CHECKING

    const PrintInstance*                m_last_instance {nullptr};
    std::optional<Point>                m_last_pos;

    std::optional<Vec3d>                m_previous_layer_last_position;
    std::optional<Vec3d>                m_previous_layer_last_position_before_wipe;
    // This needs to be populated during the layer processing!
    std::optional<Vec3d>                m_current_layer_first_position;
    double                              m_current_perimeter_extrusion_width = 0.4;
    std::optional<unsigned>             m_layer_change_extruder_id;
    // bool                                m_already_unretracted{false};
    // a previous extrusion path that is too small to be extruded, have to fusion it into the next call.
    ExtrusionPath                       m_last_too_small;
    int32_t                             m_last_command_buffer_used = 0;
    std::string                         m_last_description;
    double                              m_last_speed_mm_per_sec;

    std::unique_ptr<CoolingBuffer>      m_cooling_buffer;
    std::unique_ptr<SpiralVase>         m_spiral_vase;
    //to know the current spiral layer. Only for process_layer. began at 1, 0 means no spiral. Negative means disbaled spiral.
    int32_t                             m_spiral_vase_layer = 0;
    std::unique_ptr<GCodeFindReplace>   m_find_replace;
    std::unique_ptr<PressureEqualizer>  m_pressure_equalizer;
    std::unique_ptr<GCode::WipeTowerIntegration> m_wipe_tower;
    // to get extruded volume, for stats
    const WipeTowerData                *m_wipe_tower_data;

    // Heights (print_z) at which the skirt has already been extruded.
    std::vector<coordf_t>               m_skirt_done;
    // Has the brim been extruded already? Brim is being extruded only for the first object of a multi-object print.
    bool                                m_brim_done;
    // Flag indicating whether the nozzle temperature changes from 1st to 2nd layer were performed.
    bool                                m_second_layer_things_done;
    // G-code that is due to be written before the next extrusion
    std::string                         m_pending_pre_extrusion_gcode;
    // Pointer to currently exporting PrintObject and instance index.
    GCode::PrintObjectInstance          m_current_instance;

    // ordered list of object, to give them a unique id.
    std::vector<const PrintObject*> m_ordered_objects;
    // gcode for the start/end of the current object block.
    // as the retraction/unretraction can be written after the start/end of the algoruihtm block, it has to be delayed.
    std::string m_gcode_label_objects_start;
    std::string m_gcode_label_objects_end;
    bool m_gcode_label_objects_in_session = false;
    void _add_object_change_labels(std::string &gcode);
    void ensure_end_object_change_labels(std::string &gcode);

    bool m_silent_time_estimator_enabled;

    // Processor
    GCodeProcessor m_processor;

    //some post-processing on the file, with their data class
    std::unique_ptr<FanMover> m_fan_mover;

    std::function<void()> m_throw_if_canceled = [](){};

    double                    _compute_e_per_mm(double path_mm3_per_mm);
    std::string               _extrude(const ExtrusionPath &path, const std::string_view description, double speed = -1);
    void                      _extrude_line(std::string& gcode_str, const Line& line, const double e_per_mm, const std::string_view comment, ExtrusionRole role);
    void                      _extrude_line_cut_corner(std::string& gcode_str, const Line& line, const double e_per_mm, const std::string_view comment, Point& last_pos, const double path_width);
    std::string               _before_extrude(const ExtrusionPath &path, const std::string_view description, double speed = -1);
    double_t                  _compute_speed_mm_per_sec(const ExtrusionPath &path_attrs, double speed, double &fan_speed, std::string *comment);
    std::pair<double, double> _compute_acceleration(const ExtrusionPath &path);
    std::string               _after_extrude(const ExtrusionPath &path);
    void print_machine_envelope(GCodeOutputStream &file, const Print &print);
    void _print_first_layer_bed_temperature(std::string &out, const Print &print, const std::string &gcode, uint16_t first_printing_extruder_id, bool wait);
    void _print_first_layer_chamber_temperature(std::string &out, const Print &print, const std::string &gcode, uint16_t first_printing_extruder_id, bool wait);
    void _print_first_layer_extruder_temperatures(std::string &out, const Print &print, const std::string &gcode, uint16_t first_printing_extruder_id, bool wait);
    // On the first printing layer. This flag triggers first layer speeds.
    bool                                on_first_layer() const { return m_layer != nullptr && m_layer->id() == 0; }
    // To control print speed of 1st object layer over raft interface.
    bool                                object_layer_over_raft() const { return m_object_layer_over_raft; }

    friend class GCode::Wipe;
    friend class GCode::WipeTowerIntegration;
    friend class PressureEqualizer;

    //utility for cooling markers
    static inline std::string _cooldown_marker_speed[uint8_t(GCodeExtrusionRole::Count)];
    bool cooldwon_marker_no_slowdown_section = false;;
    static void cooldown_marker_init();
};

std::vector<const PrintInstance*> sort_object_instances_by_model_order(const Print& print);

}

#endif
