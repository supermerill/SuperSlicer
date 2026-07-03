///|/ Copyright (c) SuperSlicer 2026 Durand Rémi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/

#include "DefaultSkirtGenerator.hpp"

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <iterator>
#include <set>
#include <stdexcept>
#include <utility>
#include <vector>

#include "libslic3r/Api/plugin/c/steps/slic3r_step_skirt_brim.h"
#include "libslic3r/Api/plugin/cpp/ClipperViews.hpp"
#include "libslic3r/Api/plugin/cpp/PluginBase.hpp"
#include "libslic3r/Api/plugin/cpp/PrintHelpers.hpp"
#include "libslic3r/Api/plugin/cpp/SkirtBrimStepViews.hpp"

/*
Default skirt generator
=======================

The classic skirt code had direct access to Print internals, Flow, Extruder and
ClipperUtils. This plugin follows the same algorithmic shape, but each host
dependency has an API-side replacement:

- Print/Object/Layer views provide slices, support layers, instances and config;
- PrintHelpers reproduces the first-layer height, skirt flow and E/mm logic;
- ClipperViews builds offset loops around the convex hull;
- SkirtBrimStep moves the final extrusion trees and hull points into Print.

The plugin deliberately generates skirt after brim. When skirt_distance_from_brim
is enabled, the already-published brim points become part of the skirt hull, so
the skirt stands outside the real adhesion geometry rather than estimating brim
from settings.
*/

namespace slic3r_api { namespace SkirtBrim { namespace DefaultSkirtGeneratorPlugin {
namespace {

const char *const k_no_dependencies[] = { nullptr };
const char *const k_group_id = "skirt_brim.skirt";
constexpr double k_rounding_overlap = 1.0 - 0.25 * PI;

const raw_used_config_key k_used_config_keys[] = {
    { "skirts", RAW_CO_INT, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE },
    { "skirt_height", RAW_CO_INT, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE },
    { "skirt_distance", RAW_CO_FLOAT, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE },
    { "skirt_distance_from_brim", RAW_CO_BOOL, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE },
    { "skirt_brim", RAW_CO_INT, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE },
    { "min_skirt_length", RAW_CO_FLOAT, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE },
    { "draft_shield", RAW_CO_ENUM, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE },
    { "complete_objects", RAW_CO_BOOL, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE },
    { "complete_objects_one_skirt", RAW_CO_BOOL, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE },
    { "resolution_internal", RAW_CO_FLOAT, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE },
    { "first_layer_height", RAW_CO_FLOAT_OR_PERCENT, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE },
    { "skirt_extrusion_width", RAW_CO_FLOAT_OR_PERCENT, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE },
    { "first_layer_extrusion_width", RAW_CO_FLOAT_OR_PERCENT, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE },
    { "first_layer_extrusion_spacing", RAW_CO_FLOAT_OR_PERCENT, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE },
    { "perimeter_extrusion_width", RAW_CO_FLOAT_OR_PERCENT, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE },
    { "perimeter_extrusion_spacing", RAW_CO_FLOAT_OR_PERCENT, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE },
    { "extrusion_width", RAW_CO_FLOAT_OR_PERCENT, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE },
    { "extrusion_spacing", RAW_CO_FLOAT_OR_PERCENT, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE },
    { "nozzle_diameter", RAW_CO_VECTOR_FLOAT, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE },
    { "filament_diameter", RAW_CO_VECTOR_FLOAT, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE },
    { "extrusion_multiplier", RAW_CO_VECTOR_FLOAT, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE },
    { "use_volumetric_e", RAW_CO_BOOL, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE },
    { "perimeters", RAW_CO_INT, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE },
    { "brim_width", RAW_CO_FLOAT, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE },
    { "brim_width_interior", RAW_CO_FLOAT, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE },
    { "fill_density", RAW_CO_PERCENT, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE },
    { "top_solid_layers", RAW_CO_INT, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE },
    { "bottom_solid_layers", RAW_CO_INT, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE },
    { "solid_infill_every_layers", RAW_CO_INT, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE },
    { "perimeter_extruder", RAW_CO_INT, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE },
    { "infill_extruder", RAW_CO_INT, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE },
    { "solid_infill_extruder", RAW_CO_INT, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE },
    { "support_material_extruder", RAW_CO_INT, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE },
    { "support_material_interface_extruder", RAW_CO_INT, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE },
    { "support_material_interface_layers", RAW_CO_INT, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE }
};

struct SkirtOutput
{
    StoredExtrusionEntity skirt;
    StoredExtrusionEntity first_layer;
    StoredPolygonCollection convex_hull_points;

    explicit SkirtOutput(storage_handle *storage) :
        skirt(storage),
        first_layer(storage),
        convex_hull_points(storage)
    {
        /*
        The generated loop order is meaningful: the host should not sort these
        transport roots before G-code planning sees them.
        */
        skirt.disable_sort().disable_reverse();
        first_layer.disable_sort().disable_reverse();
    }
};

// Return true when config values request any skirt-like output.
bool has_skirt_work(const Config &print_config);

// Return true for draft-shield mode where the skirt is intentionally tall.
bool has_infinite_skirt(const Config &print_config);

// Resolve the width/spacing settings exactly as the classic skirt flow did.
c_float_or_percent effective_skirt_width_option(const Config &print_config);
c_float_or_percent effective_skirt_spacing_option(const Config &print_config);
c_flow make_skirt_flow_from_options(c_float_or_percent width_option,
                                    c_float_or_percent spacing_option,
                                    double nozzle_diameter,
                                    double height);
c_flow skirt_flow(const Print &print, uint16_t extruder_id);

// Copy one point range into a normal std::vector used as the plugin point cloud.
void append_points_from_polygon(std::vector<c_point> &points, const Polygon &polygon);

// Add every point from an extrusion tree; empty optional trees are ignored.
void append_points_from_extrusion(std::vector<c_point> &points, const ExtrusionEntity &entity);

// Prefer structured object-brim auxiliary layers and return whether any brim points were found.
bool append_points_from_object_brim_auxiliary_layers(std::vector<c_point> &points, const Object &object);

// Collect object/support/brim points in object-local coordinates.
void collect_object_local_hull_points(std::vector<c_point> &object_points,
                                      storage_handle *storage,
                                      const SkirtBrimStep &step,
                                      const Print &print,
                                      const Object &object,
                                      coord_t skirt_height_z);

// Collect all points that define the final print-level skirt hull.
StoredPolygon collect_print_hull(storage_handle *storage,
                                 const SkirtBrimStep &step,
                                 const Print &print,
                                 const std::vector<Object> &objects,
                                 bool object_local);

// Build one skirt loop entity from an offset polygon and flow.
StoredExtrusionEntity make_skirt_loop(storage_handle *storage,
                                      const Polygon &loop,
                                      const c_flow &flow,
                                      coord_t min_first_layer_height);

// Generate all skirt and first-layer-only loops around one convex hull.
SkirtOutput make_skirt(storage_handle *storage,
                       const SkirtBrimStep &step,
                       const Print &print,
                       const std::vector<Object> &objects,
                       bool object_local);

// Publish a print-owned or object-owned skirt result through the step callbacks.
void publish_print_skirt(const SkirtBrimStep &step, SkirtOutput &output);
void publish_object_skirt(const SkirtBrimStep &step, const Object &object, SkirtOutput &output);

// Generate the correct global/per-object branch for one print.
void generate_default_skirt(const plugin_run_context *run_ctx);

bool has_skirt_work(const Config &print_config)
{
    return (print_config.get("skirt_height").get_int() > 0 && print_config.get("skirts").get_int() > 0) ||
           has_infinite_skirt(print_config) ||
           print_config.get("draft_shield").get_int() != 0 ||
           print_config.get("skirt_brim").get_int() > 0;
}

bool has_infinite_skirt(const Config &print_config)
{
    return print_config.get("draft_shield").get_int() == 2 && print_config.get("skirts").get_int() > 0;
}

c_float_or_percent effective_skirt_width_option(const Config &print_config)
{
    ConfigOption option = print_config.get("skirt_extrusion_width");
    c_float_or_percent out = option.get_float_or_percent();

    /*
    A one-layer draft shield uses the first-layer width before falling back to
    the normal perimeter/extrusion width chain. That keeps the shield printable
    with the same first-layer tuning as the rest of the adhesion geometry.
    */
    if (out.value == 0.0 && print_config.float_or_default("first_layer_extrusion_width", 0.0) > 0.0 &&
        print_config.int_or_default("skirt_height", 0) == 1 &&
        print_config.enum_or_default("draft_shield", 0) != 0)
        out = print_config.get("first_layer_extrusion_width").get_float_or_percent();

    if (out.value == 0.0 && print_config.has("perimeter_extrusion_width"))
        out = print_config.get("perimeter_extrusion_width").get_float_or_percent();
    if (out.value == 0.0 && print_config.has("extrusion_width"))
        out = print_config.get("extrusion_width").get_float_or_percent();
    return out;
}

c_float_or_percent effective_skirt_spacing_option(const Config &print_config)
{
    ConfigOption option = print_config.get("skirt_extrusion_width");
    c_float_or_percent out = option.get_float_or_percent();

    /*
    Spacing has its own first-layer fallback. Width and spacing can be tuned
    independently, so the plugin resolves both options before building the
    rounded-rectangle flow used by skirt paths.
    */
    if (out.value == 0.0 && print_config.float_or_default("first_layer_extrusion_spacing", 0.0) > 0.0 &&
        print_config.int_or_default("skirt_height", 0) == 1 &&
        print_config.enum_or_default("draft_shield", 0) != 0)
        out = print_config.get("first_layer_extrusion_spacing").get_float_or_percent();

    if (out.value == 0.0 && print_config.has("perimeter_extrusion_spacing"))
        out = print_config.get("perimeter_extrusion_spacing").get_float_or_percent();
    if (out.value == 0.0 && print_config.has("extrusion_spacing"))
        out = print_config.get("extrusion_spacing").get_float_or_percent();
    return out;
}

c_flow make_skirt_flow_from_options(const c_float_or_percent width_option,
                                    const c_float_or_percent spacing_option,
                                    const double nozzle_diameter,
                                    const double height)
{
    if (height <= 0.0)
        throw std::runtime_error("Cannot compute skirt flow without a positive first-layer height.");

    double width = (!width_option.percent && width_option.value == 0.0) ?
        1.125 * nozzle_diameter :
        c_float_or_percent_get_effective_value(&width_option, nozzle_diameter);
    double spacing = width - height * k_rounding_overlap;

    /*
    A percentage width equal to zero means "auto width". If the user provided a
    concrete spacing instead, the classic flow keeps that spacing and derives
    the physical width needed to obtain it.
    */
    if (width_option.percent && width_option.value == 0.0 && spacing_option.value != 0.0) {
        spacing = c_float_or_percent_get_effective_value(&spacing_option, nozzle_diameter);
        width = spacing + height * k_rounding_overlap;
    }

    if (spacing <= 0.0 || width <= 0.0)
        throw std::runtime_error("Skirt flow produced a non-positive width or spacing.");

    c_flow out = {};
    out.width = scale_i(width);
    out.spacing = scale_i(spacing);
    out.height = scale_to_layer_coord(height);
    out.nozzle_diameter = scale_i(nozzle_diameter);
    out.spacing_ratio = 1.0f;
    out.is_bridge = 0;
    out.mm3_per_mm = height * (width - height * k_rounding_overlap);
    return out;
}

c_flow skirt_flow(const Print &print, const uint16_t extruder_id)
{
    const Config print_config = print.config();

    /*
    The skirt is a print-level object. The classic code used the largest nozzle
    that can participate in first-layer printing, so all skirt loops share one
    conservative width even when several objects or supports use different
    extruders.
    */
    double max_nozzle_diameter = 0.0;
    for (uint32_t object_idx = 0; object_idx < print.object_count(); ++object_idx) {
        const Object object = print.object(object_idx);
        const std::set<uint16_t> extruders = object_extruders(print, object);
        for (uint16_t object_extruder : extruders)
            max_nozzle_diameter = std::max(
                max_nozzle_diameter,
                print_config.vector_float_or_default("nozzle_diameter", object_extruder, 0.4));
    }
    if (max_nozzle_diameter <= 0.0)
        max_nozzle_diameter = print_config.vector_float_or_default("nozzle_diameter", extruder_id, 0.4);

    return make_skirt_flow_from_options(
        effective_skirt_width_option(print_config),
        effective_skirt_spacing_option(print_config),
        max_nozzle_diameter,
        unscaled(get_min_first_layer_height(print)));
}

void append_points_from_polygon(std::vector<c_point> &points, const Polygon &polygon)
{
    const std::vector<c_point> polygon_points = polygon.points();
    points.insert(points.end(), polygon_points.begin(), polygon_points.end());
}

void append_points_from_extrusion(std::vector<c_point> &points, const ExtrusionEntity &entity)
{
    if (entity.empty())
        return;
    const std::vector<c_point> extrusion_points = entity.collect_points();
    points.insert(points.end(), extrusion_points.begin(), extrusion_points.end());
}

bool append_points_from_object_brim_auxiliary_layers(std::vector<c_point> &points, const Object &object)
{
    bool found = false;
    for (uint32_t layer_idx = 0; layer_idx < object.auxiliary_layer_count(); ++layer_idx) {
        const Layer layer = object.auxiliary_layer(layer_idx);
        if (layer.properties().get<LayerBrimProperty>() == nullptr)
            continue;

        /*
        Object brim is now stored as normal perimeter-bucket extrusions inside
        auxiliary layer region-islands. Reading that tree keeps the skirt hull
        tied to the structured output instead of the temporary m_brim mirror.
        */
        for (uint32_t island_idx = 0; island_idx < layer.island_count(); ++island_idx) {
            const LayerIsland island = layer.island(island_idx);
            for (uint32_t region_island_idx = 0; region_island_idx < island.region_island_count();
                 ++region_island_idx) {
                const LayerRegionIsland region_island = island.region_island(region_island_idx);
                if (!region_island.has_extrusion(RAW_EXTRUSION_ROLE_PERIMETER))
                    continue;
                append_points_from_extrusion(
                    points,
                    ExtrusionEntity(region_island.extrusion(RAW_EXTRUSION_ROLE_PERIMETER)));
                found = true;
            }
        }
    }
    return found;
}

void collect_object_local_hull_points(std::vector<c_point> &object_points,
                                      storage_handle *storage,
                                      const SkirtBrimStep &step,
                                      const Print &print,
                                      const Object &object,
                                      const coord_t skirt_height_z)
{
    /*
    The skirt hull is based on visible outer contours only. Holes are ignored:
    the skirt is outside the object/support envelope, so internal voids cannot
    shrink the required boundary.
    */
    for (uint32_t layer_idx = 0; layer_idx < object.layer_count(); ++layer_idx) {
        const Layer layer = object.layer(layer_idx);
        if (layer.print_z() > skirt_height_z)
            break;
        for (ExPolygon expolygon : layer.slices())
            append_points_from_polygon(object_points, expolygon.contour());
    }

    /*
    Support layers are treated like object geometry for skirt placement. They
    are already generated before STEP_SKIRT_BRIM in the new pipeline, so no
    support-specific host shortcut is needed here.
    */
    for (uint32_t layer_idx = 0; layer_idx < object.auxiliary_layer_count(); ++layer_idx) {
        const Layer support_layer = object.auxiliary_layer(layer_idx);
        if (support_layer.properties().get<LayerSupportProperty>() == nullptr)
            continue;
        if (support_layer.print_z() > skirt_height_z)
            break;
        for (ExPolygon expolygon : support_layer.slices())
            append_points_from_polygon(object_points, expolygon.contour());
    }

    if (print.config().get("skirt_distance_from_brim").get_bool()) {
        if (!append_points_from_object_brim_auxiliary_layers(object_points, object))
            append_points_from_extrusion(object_points, step.object_brim(object));

        /*
        In object-local mode, old brim patches are now represented by the brim
        plugin's extrusion output. Using those points avoids a new volume API and
        keeps skirt placement tied to the actual brim that will be printed.
        */
        StoredPolygon object_polygon(storage);
        for (c_point point : object_points)
            object_polygon.push_back(point);
        if (object_polygon.size() >= 3) {
            const double brim_width = object.config().get("brim_width").get_float();
            const ClipperContext clipper(storage);
            const StoredPolygon object_hull = convex_hull(storage, object_polygon.readonly());
            const StoredPolygonCollection widened =
                clipper_offset(clipper(object_hull.readonly()), scale_d(brim_width), CLIPPER_JOIN_ROUND,
                               double(std::max<coord_t>(SCALED_EPSILON, skirt_flow(print, 0).width / 10)))
                    .to_polygon_collection();
            for (Polygon polygon : widened)
                append_points_from_polygon(object_points, polygon);
        }
    }
}

StoredPolygon collect_print_hull(storage_handle *storage,
                                 const SkirtBrimStep &step,
                                 const Print &print,
                                 const std::vector<Object> &objects,
                                 const bool object_local)
{
    const Config print_config = print.config();
    coord_t skirt_height_z = 0;
    for (Object object : objects) {
        if (object.layer_count() == 0)
            continue;
        const uint32_t layer_count = has_infinite_skirt(print_config) ?
            object.layer_count() :
            std::min<uint32_t>(uint32_t(std::max(1, print_config.get("skirt_height").get_int())),
                               object.layer_count());
        skirt_height_z = std::max(skirt_height_z, object.layer(layer_count - 1).print_z());
    }

    std::vector<c_point> points;
    for (Object object : objects) {
        std::vector<c_point> object_points;
        collect_object_local_hull_points(object_points, storage, step, print, object, skirt_height_z);

        /*
        Slices are object-local. A print-level skirt must cover every object
        instance, so it translates the local hull by each instance shift. An
        object-owned skirt is stored in object coordinates, so it deliberately
        keeps a single origin copy and lets the object instance transform apply
        later.
        */
        if (object_points.size() >= 3) {
            StoredPolygon local_polygon(storage);
            for (c_point point : object_points)
                local_polygon.push_back(point);
            StoredPolygon local_hull = convex_hull(storage, local_polygon.readonly());
            object_points = local_hull.points();
        }

        const uint32_t instance_count = object_local ? 1 : object.instance_count();
        for (uint32_t instance_idx = 0; instance_idx < instance_count; ++instance_idx) {
            const c_point shift = object_local ? c_point{} : object.instance_shift(instance_idx);
            for (c_point point : object_points) {
                point.x += shift.x;
                point.y += shift.y;
                points.push_back(point);
            }
        }
    }

    if (print_config.get("draft_shield").get_int() == 0 || print_config.get("skirt_distance_from_brim").get_bool()) {
        append_points_from_extrusion(points, step.brim());
        for (uint32_t object_idx = 0; object_idx < print.object_count(); ++object_idx) {
            const Object object = print.object(object_idx);
            if (!append_points_from_object_brim_auxiliary_layers(points, object))
                append_points_from_extrusion(points, step.object_brim(object));
        }
    }

    StoredPolygon point_cloud(storage);
    for (c_point point : points)
        point_cloud.push_back(point);
    return point_cloud.size() < 3 ? StoredPolygon(storage) : convex_hull(storage, point_cloud.readonly());
}

StoredExtrusionEntity make_skirt_loop(storage_handle *storage,
                                      const Polygon &loop,
                                      const c_flow &flow,
                                      const coord_t min_first_layer_height)
{
    std::vector<c_point> points = loop.points();
    if (points.empty())
        return StoredExtrusionEntity(storage);
    points.push_back(points.front());

    StoredExtrusionEntity out(storage);
    if (!out.set_points(points) || out.point_count() != points.size())
        throw std::runtime_error("Default skirt generator could not create a skirt loop polyline.");
    out.disable_sort().disable_reverse();
    EPropertyAttributes &attributes = out.get_or_add_property<EPropertyAttributes>();
    attributes.extrusion_role(RAW_EXTRUSION_ROLE_SKIRT)
        .mm3_per_mm(flow.mm3_per_mm)
        .width(float(unscaled(flow.width)))
        .height(float(unscaled(min_first_layer_height)));
    out.get_or_add_property<EPropertyPerimeter>()
        .shell_count(0)
        .perimeter_flags(C_EXTRUSION_PERIMETER_FLAG_LOOP | C_EXTRUSION_PERIMETER_FLAG_SKIRT);

    if (loop.is_clockwise())
        out.reverse();
    return out;
}

SkirtOutput make_skirt(storage_handle *storage,
                       const SkirtBrimStep &step,
                       const Print &print,
                       const std::vector<Object> &objects,
                       const bool object_local)
{
    SkirtOutput output(storage);
    const Config print_config = print.config();
    StoredPolygon convex_hull_polygon = collect_print_hull(storage, step, print, objects, object_local);
    if (convex_hull_polygon.size() < 3)
        return output;

    std::vector<uint16_t> extruders;
    const std::set<uint16_t> extruder_set = collect_print_extruders_for_skirt(print, objects);
    extruders.insert(extruders.end(), extruder_set.begin(), extruder_set.end());
    if (extruders.empty())
        extruders.push_back(0);

    uint32_t n_skirts = uint32_t(std::max(0, print_config.get("skirts").get_int()));
    const uint32_t n_skirts_first_layer = n_skirts + uint32_t(std::max(0, print_config.get("skirt_brim").get_int()));
    if (has_infinite_skirt(print_config) && n_skirts == 0)
        n_skirts = 1;

    const uint32_t max_loop_count = std::max(n_skirts, n_skirts_first_layer);
    if (max_loop_count == 0)
        return output;

    const c_flow last_flow = skirt_flow(print, extruders.back());
    coord_t distance = scale_i(print_config.get("skirt_distance").get_float()) - last_flow.spacing / 2;
    const coord_t min_first_layer_height = get_min_first_layer_height(print);
    const uint32_t lines_per_extruder =
        uint32_t((n_skirts + uint32_t(extruders.size()) - 1) / uint32_t(extruders.size()));
    uint32_t current_lines_per_extruder =
        n_skirts - lines_per_extruder * (uint32_t(extruders.size()) - 1);
    std::vector<double> extruded_length(extruders.size(), 0.0);

    /*
    The legacy algorithm walks from the inner line outward, then reverses the
    output so printing starts at the outermost skirt. Keeping that order makes
    min_skirt_length and multi-extruder distribution match the classic path.
    Instead of storing every temporary loop and reversing later, each new loop
    is inserted at child index zero. The final tree therefore has the same
    outer-to-inner order without keeping extra storage-owned entities alive.
    */
    for (uint32_t remaining = max_loop_count, extruder_idx = 0, nb_skirts = 1; remaining > 0; --remaining) {
        const bool first_layer_only = remaining <= (n_skirts_first_layer - n_skirts);
        const uint16_t extruder_id = extruders[extruders.size() - (1 + extruder_idx)];
        const c_flow flow = skirt_flow(print, extruder_id);
        distance += flow.spacing / 2;

        const ClipperContext clipper(storage);
        StoredPolygonCollection loops =
            clipper_offset(clipper(convex_hull_polygon.readonly()), double(distance), CLIPPER_JOIN_ROUND,
                           double(std::max<coord_t>(SCALED_EPSILON, flow.width / 10)))
                .to_polygon_collection();
        uint32_t loop_idx = 0;
        for (; loop_idx < loops.size(); ++loop_idx) {
            Polygon candidate = loops[loop_idx];
            if (!candidate.empty() && candidate.is_valid())
                break;
        }
        if (loop_idx == loops.size())
            break;

        Polygon loop = loops[loop_idx];
        StoredExtrusionEntity entity = make_skirt_loop(storage, loop, flow, min_first_layer_height);
        distance += flow.spacing / 2;

        if (!entity.empty()) {
            if (n_skirts_first_layer > n_skirts) {
                if (first_layer_only) {
                    const uint32_t inserted_idx = output.first_layer.insert_child_move(0, entity.mutable_view());
                    if (is_invalid_index(inserted_idx))
                        throw std::runtime_error("Default skirt generator could not add first-layer skirt loop.");
                } else {
                    StoredExtrusionEntity first_layer_entity(storage, entity.readonly());
                    const uint32_t inserted_idx = output.first_layer.insert_child_move(0, first_layer_entity.mutable_view());
                    if (is_invalid_index(inserted_idx))
                        throw std::runtime_error("Default skirt generator could not add first-layer skirt copy.");
                }
            }

            if (!first_layer_only) {
                const uint32_t inserted_idx = output.skirt.insert_child_move(0, entity.mutable_view());
                if (is_invalid_index(inserted_idx))
                    throw std::runtime_error("Default skirt generator could not add skirt loop.");
            }

            /*
            The first child insertion turns an empty entity into a generic
            collection and the host default for such collections is sortable.
            Skirt loops are intentionally ordered from outside to inside, so
            reapply the step contract after insertions have created children.
            */
            output.skirt.disable_sort().disable_reverse();
            output.first_layer.disable_sort().disable_reverse();
        }

        if (print_config.get("min_skirt_length").get_float() > 0.0 && !first_layer_only) {
            extruded_length[extruder_idx] += unscaled(loop.length()) * e_per_mm(print, extruder_id, flow.mm3_per_mm);
            if (extruded_length[extruder_idx] < print_config.get("min_skirt_length").get_float()) {
                if (remaining == 1 && extruded_length[extruder_idx] > 0.0)
                    ++remaining;
            } else if (extruder_idx + 1 < extruders.size()) {
                if (nb_skirts < current_lines_per_extruder) {
                    ++nb_skirts;
                } else {
                    current_lines_per_extruder = lines_per_extruder;
                    nb_skirts = 1;
                    ++extruder_idx;
                }
            }
        }
    }

    const c_flow hull_flow = skirt_flow(print, extruders.back());
    output.convex_hull_points =
        clipper_offset(ClipperContext(storage)(convex_hull_polygon.readonly()),
                       double(distance + hull_flow.spacing / 2), CLIPPER_JOIN_ROUND, double(scale_d(0.1)))
            .to_polygon_collection();
    return output;
}

void publish_print_skirt(const SkirtBrimStep &step, SkirtOutput &output)
{
    if (!output.skirt.empty() && !step.append_skirt_move(output.skirt))
        throw std::runtime_error("Default skirt generator could not publish print skirt.");
    if (!output.first_layer.empty() && !step.append_skirt_first_layer_move(output.first_layer))
        throw std::runtime_error("Default skirt generator could not publish first-layer skirt.");
    if (!output.convex_hull_points.empty() && !step.append_skirt_convex_hull_move(output.convex_hull_points))
        throw std::runtime_error("Default skirt generator could not publish skirt hull.");
}

void publish_object_skirt(const SkirtBrimStep &step, const Object &object, SkirtOutput &output)
{
    if (!output.skirt.empty() && !step.append_object_skirt_move(object, output.skirt))
        throw std::runtime_error("Default skirt generator could not publish object skirt.");
    if (!output.first_layer.empty() && !step.append_object_skirt_first_layer_move(object, output.first_layer))
        throw std::runtime_error("Default skirt generator could not publish object first-layer skirt.");
    if (!output.convex_hull_points.empty() && !step.append_skirt_convex_hull_move(output.convex_hull_points))
        throw std::runtime_error("Default skirt generator could not publish object skirt hull.");
}

void generate_default_skirt(const plugin_run_context *run_ctx)
{
    const run_ctx_skirt_brim *ctx = plugin_ctx_as_skirt_brim(run_ctx);
    if (ctx == nullptr || ctx->print == nullptr)
        return;

    const SkirtBrimStep step(ctx);
    const Print print = step.print();
    const Config print_config = print.config();
    if (!has_skirt_work(print_config))
        return;

    storage_handle *storage = run_ctx->plugin_storage;
    if (print_config.get("complete_objects").get_bool() &&
        !print_config.get("complete_objects_one_skirt").get_bool()) {
        for (uint32_t object_idx = 0; object_idx < print.object_count(); ++object_idx) {
            const Object object = print.object(object_idx);
            SkirtOutput output = make_skirt(storage, step, print, std::vector<Object>{ object }, true);
            publish_object_skirt(step, object, output);
        }
        return;
    }

    std::vector<Object> objects;
    objects.reserve(print.object_count());
    for (uint32_t object_idx = 0; object_idx < print.object_count(); ++object_idx)
        objects.push_back(print.object(object_idx));
    SkirtOutput output = make_skirt(storage, step, print, objects, false);
    publish_print_skirt(step, output);
}

class DefaultSkirtGenerator : public PluginBase
{
public:
    static DefaultSkirtGenerator &instance(orchestrator_handle *orchestrator)
    {
        static DefaultSkirtGenerator instance(orchestrator);
        return instance;
    }

    explicit DefaultSkirtGenerator(orchestrator_handle *orchestrator) : PluginBase(orchestrator) {}

private:
    const char *id_impl() const noexcept override { return "skirt_brim.skirt.default"; }
    const char *name_impl() const noexcept override { return "Default skirt generator"; }
    const char *description_impl() const noexcept override
    {
        return "Generates the classic skirt and first-layer skirt-brim loops using only the plugin API.";
    }
    const char *exclusive_group_impl() const noexcept override { return k_group_id; }
    const char *exclusive_group_label_impl() const noexcept override { return "Skirt generator"; }
    const char *exclusive_group_tooltip_impl() const noexcept override
    {
        return "Selects which plugin generates skirt extrusion around the first-layer envelope.";
    }
    slicing_step_t step_impl() const noexcept override { return STEP_SKIRT_BRIM; }
    const char *const *dependencies_impl() const noexcept override { return k_no_dependencies; }
    int32_t priority_impl() const noexcept override { return 100; }
    const char *progress_message_format_impl() const noexcept override { return "Generating skirt"; }

    int32_t used_config_keys(raw_used_config_key *keys) const noexcept override
    {
        if (keys == nullptr)
            return int32_t(std::size(k_used_config_keys));
        std::copy(std::begin(k_used_config_keys), std::end(k_used_config_keys), keys);
        return int32_t(std::size(k_used_config_keys));
    }

    void run_impl(const plugin_run_context *run_ctx) const override
    {
        generate_default_skirt(run_ctx);
    }
};

} // namespace

void register_default_skirt_generator_plugin(orchestrator_handle *orchestrator)
{
    orchestrator_register_plugin(orchestrator, DefaultSkirtGenerator::instance(orchestrator).c_instance());
}

}}} // namespace slic3r_api::SkirtBrim::DefaultSkirtGeneratorPlugin
