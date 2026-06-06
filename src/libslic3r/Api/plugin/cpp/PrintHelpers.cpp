///|/ Copyright (c) SuperSlicer 2026 Durand Rémi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/
#include "PrintHelpers.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace slic3r_api {
namespace {

uint16_t normalize_extruder_id(const Config &print_config, int32_t one_based_id)
{
    const ConfigOption nozzle_diameter = print_config.get("nozzle_diameter");
    const uint32_t num_extruders = nozzle_diameter.size();
    const int32_t idx = std::max<int32_t>(0, one_based_id - 1);
    return static_cast<uint16_t>((num_extruders == 0 || uint32_t(idx) >= num_extruders) ? 0 : idx);
}

void append_support_extruders(const Print &print,
                              const std::vector<Object> &objects,
                              std::set<uint16_t> &extruders)
{
    const Config print_config = print.config();
    bool support_uses_current_extruder = false;

    for (Object object : objects) {
        bool has_support_auxiliary = false;
        for (uint32_t layer_idx = 0; layer_idx < object.auxiliary_layer_count(); ++layer_idx)
            if (object.auxiliary_layer(layer_idx).properties().get<LayerSupportProperty>() != nullptr) {
                has_support_auxiliary = true;
                break;
            }
        if (!has_support_auxiliary)
            continue;

        const Config object_config = object.config();
        const int32_t support_extruder = object_config.int_or_default("support_material_extruder", 0);
        const int32_t interface_extruder = object_config.int_or_default("support_material_interface_extruder", 0);

        if (support_extruder == 0)
            support_uses_current_extruder = true;
        else
            extruders.insert(normalize_extruder_id(print_config, support_extruder));

        if (object_config.int_or_default("support_material_interface_layers", 0) > 0) {
            if (interface_extruder == 0)
                support_uses_current_extruder = true;
            else
                extruders.insert(normalize_extruder_id(print_config, interface_extruder));
        }
    }

    if (support_uses_current_extruder)
        for (Object object : objects) {
            const std::set<uint16_t> object_tools = object_extruders(print, object);
            extruders.insert(object_tools.begin(), object_tools.end());
        }
}

} // namespace

coord_t check_z_step(coord_t val, coord_t z_step)
{
    if (z_step <= SCALED_EPSILON)
        return val;
    return ((((val * 2) + z_step) / (2 * z_step)) * z_step);
}

void collect_object_printing_extruders(const Print &print,
                                       const Object &object,
                                       const PrintRegion &region,
                                       std::set<uint16_t> &object_extruders)
{
    const ConfigOption nozzle_diameter = print.config().get("nozzle_diameter");
    const int num_extruders = static_cast<int>(nozzle_diameter.size());

    auto emplace_extruder = [num_extruders, &object_extruders](int extruder_id) {
        const int idx = std::max(0, extruder_id - 1);
        object_extruders.insert(static_cast<uint16_t>((idx >= num_extruders) ? 0 : idx));
    };

    const Config object_config = object.config();
    const Config region_config = region.config();

    if (region_config.get("perimeters").get_int() > 0 ||
        object_config.get("brim_width").get_float() > 0.0 ||
        object_config.get("brim_width_interior").get_float() > 0.0)
        emplace_extruder(region_config.get("perimeter_extruder").get_int());

    if (region_config.get("fill_density").get_float() > 0.0)
        emplace_extruder(region_config.get("infill_extruder").get_int());

    if (region_config.get("top_solid_layers").get_int() > 0 ||
        region_config.get("bottom_solid_layers").get_int() > 0 ||
        (region_config.get("solid_infill_every_layers").get_int() > 0 &&
         region_config.get("fill_density").get_float() > 0.0))
        emplace_extruder(region_config.get("solid_infill_extruder").get_int());
}

std::set<uint16_t> object_extruders(const Print &print, const Object &object)
{
    std::set<uint16_t> extruders;
    for (uint32_t region_idx = 0; region_idx < object.print_region_count(); ++region_idx)
        collect_object_printing_extruders(print, object, object.print_region(region_idx), extruders);
    return extruders;
}

coord_t get_object_first_layer_height(const Print &print, const Object &object)
{
    const Config print_config = print.config();
    const Config object_config = object.config();
    const ConfigOption first_layer_height = object_config.get("first_layer_height");

    if (!first_layer_height.is_percent())
        return scale_to_layer_coord(first_layer_height.get_float());

    coord_t out = std::numeric_limits<coord_t>::max();
    const std::set<uint16_t> extruders = object_extruders(print, object);
    for (uint16_t extruder_id : extruders) {
        const double nozzle_diameter = print_config.vector_float_or_default("nozzle_diameter", extruder_id, 0.4);
        out = std::min(out, scale_to_layer_coord(first_layer_height.get_effective_value(nozzle_diameter)));
    }

    if (out == std::numeric_limits<coord_t>::max()) {
        const double nozzle_diameter = print_config.vector_float_or_default("nozzle_diameter", 0, 0.4);
        out = scale_to_layer_coord(first_layer_height.get_effective_value(nozzle_diameter));
    }
    return out;
}

coord_t get_min_first_layer_height(const Print &print)
{
    if (print.object_count() == 0)
        throw std::runtime_error("Cannot compute first-layer height without Print objects.");

    coord_t out = std::numeric_limits<coord_t>::max();
    for (uint32_t object_idx = 0; object_idx < print.object_count(); ++object_idx)
        out = std::min(out, get_object_first_layer_height(print, print.object(object_idx)));
    if (out == std::numeric_limits<coord_t>::max())
        throw std::runtime_error("Cannot compute first-layer height.");
    return out;
}

std::set<uint16_t> collect_print_extruders_for_skirt(const Print &print,
                                                     const std::vector<Object> &objects)
{
    std::set<uint16_t> extruders;
    for (Object object : objects) {
        const std::set<uint16_t> object_tools = object_extruders(print, object);
        extruders.insert(object_tools.begin(), object_tools.end());
    }
    append_support_extruders(print, objects, extruders);
    if (extruders.empty())
        extruders.insert(0);
    return extruders;
}

double e_per_mm(const Print &print, uint16_t extruder_id, double mm3_per_mm)
{
    const Config print_config = print.config();
    const double extrusion_multiplier =
        print_config.vector_float_or_default("extrusion_multiplier", extruder_id, 1.0);
    if (print_config.bool_or_default("use_volumetric_e", false))
        return mm3_per_mm * extrusion_multiplier;

    const double filament_diameter =
        print_config.vector_float_or_default("filament_diameter", extruder_id, 1.75);
    const double filament_area = filament_diameter * filament_diameter * 0.25 * PI;
    if (filament_area <= 0.0)
        throw std::runtime_error("Cannot compute E per mm with a non-positive filament diameter.");
    return mm3_per_mm * extrusion_multiplier / filament_area;
}

} // namespace slic3r_api
