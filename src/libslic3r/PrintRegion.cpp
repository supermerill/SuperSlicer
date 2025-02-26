#include "Exception.hpp"
#include "Print.hpp"

namespace Slic3r {

// 1-based extruder identifier for this region and role.
uint16_t PrintRegion::extruder(FlowRole role, const PrintObject& object) const
{
    size_t extruder = 0;
    if (role == frPerimeter || role == frExternalPerimeter)
        extruder = m_config.perimeter_extruder;
    else if (role == frInfill)
        extruder = m_config.infill_extruder;
    else if (role == frSolidInfill || role == frTopSolidInfill)
        extruder = m_config.solid_infill_extruder;
    else if (role == frSupportMaterial)
        extruder = object.config().support_material_extruder;
    else if (role == frSupportMaterialInterface)
        extruder = object.config().support_material_interface_extruder;
    else
        throw Slic3r::InvalidArgument("Unknown role");
    return (uint16_t)extruder;
}

Flow PrintRegion::flow(const PrintObject &object, FlowRole role, double layer_height, size_t layer_id) const
{
    bool first_layer = layer_id == 0;
    ConfigOptionFloatOrPercent  config_width;
    ConfigOptionFloatOrPercent  config_spacing;
    // Get extrusion width from configuration.
    double overlap = 1.;
    double value_oddlayer = 0;
    bool is_ratio_oddlayer = false;

    // (might be an absolute value, or a percent value, or zero for auto)
     if (role == frExternalPerimeter) {
        config_width = m_config.external_perimeter_extrusion_width;
        config_spacing = m_config.external_perimeter_extrusion_spacing;

        if (layer_id > 0 && layer_id % 2 == 1 && m_config.external_perimeter_extrusion_change_odd_layers.value != 0) {
            is_ratio_oddlayer = m_config.external_perimeter_extrusion_change_odd_layers.percent;
            value_oddlayer = m_config.external_perimeter_extrusion_change_odd_layers.get_abs_value(1.);
        }

		overlap = this->config().external_perimeter_overlap.get_abs_value(1);
    } else if (role == frPerimeter) {
        config_width = m_config.perimeter_extrusion_width;
        config_spacing = m_config.perimeter_extrusion_spacing;

        if (layer_id > 0 && layer_id % 2 == 1 && m_config.perimeter_extrusion_change_odd_layers.value != 0) {
            is_ratio_oddlayer = m_config.perimeter_extrusion_change_odd_layers.percent;
            value_oddlayer = m_config.perimeter_extrusion_change_odd_layers.get_abs_value(1.);
        }

		overlap = this->config().perimeter_overlap.get_abs_value(1);
    } else if (role == frInfill) {
        config_width = m_config.infill_extrusion_width;
        config_spacing = m_config.infill_extrusion_spacing;

        if (layer_id > 0 && layer_id % 2 == 1 && m_config.infill_extrusion_change_odd_layers.value != 0) {
            is_ratio_oddlayer = m_config.infill_extrusion_change_odd_layers.percent;
            value_oddlayer = m_config.infill_extrusion_change_odd_layers.get_abs_value(1.);
        }

    } else if (role == frSolidInfill) {
        config_width = m_config.solid_infill_extrusion_width;
        config_spacing = m_config.solid_infill_extrusion_spacing;

        if (layer_id > 0 && layer_id % 2 == 1 && m_config.solid_infill_extrusion_change_odd_layers.value != 0) {
            is_ratio_oddlayer = m_config.solid_infill_extrusion_change_odd_layers.percent;
            value_oddlayer = m_config.solid_infill_extrusion_change_odd_layers.get_abs_value(1.);
        }
        
        overlap = this->config().solid_infill_overlap.get_abs_value(1);
    } else if (role == frTopSolidInfill) {
        config_width = m_config.top_infill_extrusion_width;
        config_spacing = m_config.top_infill_extrusion_spacing;
        overlap = this->config().solid_infill_overlap.get_abs_value(1);
    } else {
        throw Slic3r::InvalidArgument("Unknown role");
    }
    /*if (first_layer && object.config().first_layer_extrusion_width.value > 0) {
        config_width = object.config().first_layer_extrusion_width;
        config_spacing = object.config().first_layer_extrusion_spacing;
    }*/

    if (first_layer && m_config.first_layer_extrusion_width.value > 0) {
        config_width = m_config.first_layer_extrusion_width;
        config_spacing = m_config.first_layer_extrusion_spacing;
    }

    if (config_width.value == 0) {
        config_width = object.config().extrusion_width;
        config_spacing = object.config().extrusion_spacing;
    }
    
    // Get the configured nozzle_diameter for the extruder associated to the flow role requested.
    // Here this->extruder(role) - 1 may underflow to MAX_INT, but then the get_at() will follback to zero'th element, so everything is all right.
    double nozzle_diameter = object.print()->config().nozzle_diameter.get_at(this->extruder(role, object) - 1);
    Flow flow = Flow::new_from_config_width(role, config_width, config_spacing,(float)nozzle_diameter, (float)layer_height,
        (float)std::min(overlap, this->config().get_computed_value("filament_max_overlap", this->extruder(role, object) - 1)) );
        //bridge ? (float)m_config.bridge_flow_ratio.get_abs_value(1) : 0.0f);
    if (value_oddlayer > 0) {
        flow = flow.with_spacing(is_ratio_oddlayer ? flow.spacing() * value_oddlayer : value_oddlayer);
    }
    return flow;
}

float  PrintRegion::width(FlowRole role, bool first_layer, const PrintObject& object) const
{
    const ConfigOptionFloatOrPercent* config_width = nullptr;
    // otherwise, get extrusion width from configuration
    // (might be an absolute value, or a percent value, or zero for auto)
    if (first_layer && object.config().first_layer_extrusion_width.value > 0) {
        config_width = &object.config().first_layer_extrusion_width;
    } else if (role == frExternalPerimeter) {
        config_width = &m_config.external_perimeter_extrusion_width;
    } else if (role == frPerimeter) {
        config_width = &m_config.perimeter_extrusion_width;
    } else if (role == frInfill) {
        config_width = &m_config.infill_extrusion_width;
    } else if (role == frSolidInfill) {
        config_width = &m_config.solid_infill_extrusion_width;
    } else if (role == frTopSolidInfill) {
        config_width = &m_config.top_infill_extrusion_width;
    } else if (role == frSupportMaterial || role == frSupportMaterialInterface) {
        config_width = &object.config().support_material_extrusion_width;
    } else {
        throw Slic3r::InvalidArgument("Unknown role");
    }

    if (!config_width || config_width->value == 0)
        config_width = &object.config().extrusion_width;

    // Get the configured nozzle_diameter for the extruder associated to the flow role requested.
    // Here this->extruder(role) - 1 may underflow to MAX_INT, but then the get_at() will follback to zero'th element, so everything is all right.
    double nozzle_diameter = object.print()->config().nozzle_diameter.get_at(this->extruder(role, object) - 1);
    if (config_width->value <= 0.) {
        // If user left option to 0, calculate a sane default width.
        return float(Flow::auto_extrusion_width(role, nozzle_diameter));
    } else {
        // If user set a manual value, use it.
        return float(config_width->get_abs_value(nozzle_diameter));
    }
}

coordf_t PrintRegion::nozzle_dmr_avg(const PrintConfig &print_config) const
{
    return (print_config.nozzle_diameter.get_at(m_config.perimeter_extruder.value    - 1) + 
            print_config.nozzle_diameter.get_at(m_config.infill_extruder.value       - 1) + 
            print_config.nozzle_diameter.get_at(m_config.solid_infill_extruder.value - 1)) / 3.;
}

void PrintRegion::collect_object_printing_extruders(const PrintConfig &print_config, const PrintObjectConfig &object_config, const PrintRegionConfig &region_config, std::set<uint16_t> &object_extruders)
{
    // These checks reflect the same logic used in the GUI for enabling/disabling extruder selection fields.
    auto num_extruders = (int)print_config.nozzle_diameter.size();
    auto emplace_extruder = [num_extruders, &object_extruders](int extruder_id) {
    	int i = std::max(0, extruder_id - 1);
        object_extruders.insert((i >= num_extruders) ? 0 : i);
    };
    if (region_config.perimeters.value > 0 || object_config.brim_width.value > 0 || object_config.brim_width_interior > 0)
    	emplace_extruder(region_config.perimeter_extruder);
    if (region_config.fill_density.value > 0)
    	emplace_extruder(region_config.infill_extruder);
    if (region_config.top_solid_layers.value > 0 || region_config.bottom_solid_layers.value > 0)
    	emplace_extruder(region_config.solid_infill_extruder);
}

void PrintRegion::collect_object_printing_extruders(const Print& print, std::set<uint16_t> &object_extruders) const
{
    // PrintRegion, if used by some PrintObject, shall have all the extruders set to an existing printer extruder.
    // If not, then there must be something wrong with the Print::apply() function.
#ifndef NDEBUG
    auto num_extruders = int(print.config().nozzle_diameter.size());
    assert(this->config().perimeter_extruder.value      <= num_extruders);
    assert(this->config().infill_extruder.value         <= num_extruders);
    assert(this->config().solid_infill_extruder.value   <= num_extruders);
#endif
    for(const PrintObject * obj : print.objects())
        collect_object_printing_extruders(print.config(), obj->config(), this->config(), object_extruders);
}

}
