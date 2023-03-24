#include "Flow.hpp"
#include "I18N.hpp"
#include "Print.hpp"
#include "Layer.hpp"
#include <cmath>
#include <assert.h>

#include <boost/algorithm/string/predicate.hpp>

// Mark string for localization and translate.
#define L(s) Slic3r::I18N::translate(s)

namespace Slic3r {

FlowErrorNegativeSpacing::FlowErrorNegativeSpacing() : 
	FlowError("Flow::spacing() produced negative spacing. Did you set some extrusion width too small?") {}

FlowErrorNegativeFlow::FlowErrorNegativeFlow() :
    FlowError("Flow::mm3_per_mm() produced negative flow. Did you set some extrusion width too small?") {}

// This static method returns a sane extrusion width default.
float Flow::auto_extrusion_width(FlowRole role, float nozzle_diameter)
{
    switch (role) {
    case frSupportMaterial:
    case frSupportMaterialInterface:
    case frTopSolidInfill:
    case frExternalPerimeter:
        return 1.05f * nozzle_diameter;
    default:
    case frPerimeter:
    case frSolidInfill:
    case frInfill:
        return 1.125f * nozzle_diameter;
    }
}

// Used by the Flow::extrusion_width() funtion to provide hints to the user on default extrusion width values,
// and to provide reasonable values to the PlaceholderParser.
static inline FlowRole opt_key_to_flow_role(const std::string &opt_key)
{
 	if (opt_key == "perimeter_extrusion_width" || 
 		// or all the defaults:
 		opt_key == "extrusion_width" || opt_key == "first_layer_extrusion_width")
        return frPerimeter;
    else if (opt_key == "external_perimeter_extrusion_width")
        return frExternalPerimeter;
    else if (opt_key == "infill_extrusion_width")
        return frInfill;
    else if (opt_key == "solid_infill_extrusion_width")
        return frSolidInfill;
	else if (opt_key == "top_infill_extrusion_width")
		return frTopSolidInfill;
	else if (opt_key == "support_material_extrusion_width")
    	return frSupportMaterial;
    else 
    	throw Slic3r::RuntimeError("opt_key_to_flow_role: invalid argument");
};

static inline void throw_on_missing_variable(const std::string &opt_key, const char *dependent_opt_key) 
{
	throw FlowErrorMissingVariable((boost::format(L("Cannot calculate extrusion width for %1%: Variable \"%2%\" not accessible.")) % opt_key % dependent_opt_key).str());
}

// Used to provide hints to the user on default extrusion width values, and to provide reasonable values to the PlaceholderParser.
double Flow::extrusion_width(const std::string& opt_key, const ConfigOptionFloatOrPercent* opt, const ConfigOptionResolver& config, const unsigned int first_printing_extruder)
{
    assert(opt != nullptr);

    bool first_layer = boost::starts_with(opt_key, "first_layer_") || boost::starts_with(opt_key, "brim_");
    if (!first_layer && boost::starts_with(opt_key, "skirt_")) {
        const ConfigOptionInt* optInt = config.option<ConfigOptionInt>("skirt_height");
        const ConfigOptionBool* optBool = config.option<ConfigOptionBool>("draft_shield");
        first_layer = (optBool && optInt && optInt->value == 1 && !optBool->value);
    }

    if (opt->value == 0.) {
        // The role specific extrusion width value was set to zero, get a not-0 one (if possible)
        opt = extrusion_width_option(opt_key, config);
    }

    if (opt->percent) {
        auto opt_key_layer_height = first_layer ? "first_layer_height" : "layer_height";
        auto opt_layer_height = config.option(opt_key_layer_height);
        if (opt_layer_height == nullptr)
            throw_on_missing_variable(opt_key, opt_key_layer_height);
        // first_layer_height depends on first_printing_extruder
        auto opt_nozzle_diameters = config.option<ConfigOptionFloats>("nozzle_diameter");
        if (opt_nozzle_diameters == nullptr)
            throw_on_missing_variable(opt_key, "nozzle_diameter");
        return opt->get_abs_value(float(opt_nozzle_diameters->get_at(first_printing_extruder)));
    }

    if (opt->value == 0.) {
        // If user left option to 0, calculate a sane default width.
        auto opt_nozzle_diameters = config.option<ConfigOptionFloats>("nozzle_diameter");
        if (opt_nozzle_diameters == nullptr)
            throw_on_missing_variable(opt_key, "nozzle_diameter");
        return auto_extrusion_width(opt_key_to_flow_role(opt_key), float(opt_nozzle_diameters->get_at(first_printing_extruder)));
    }

    return opt->value;
}

//used to get brim & skirt extrusion config
const ConfigOptionFloatOrPercent* Flow::extrusion_width_option(std::string opt_key, const ConfigOptionResolver& config)
{
    if (!boost::ends_with(opt_key, "_extrusion_width")) {
        opt_key += "_extrusion_width";
    }

    const ConfigOptionFloatOrPercent* opt = config.option<ConfigOptionFloatOrPercent>(opt_key);

    //brim is first_layer_extrusion_width then perimeter_extrusion_width
    if (!opt && boost::starts_with(opt_key, "brim")) {
        const ConfigOptionFloatOrPercent* optTest = config.option<ConfigOptionFloatOrPercent>("first_layer_extrusion_width");
        opt = optTest;
        if (opt == nullptr)
            throw_on_missing_variable(opt_key, "first_layer_extrusion_width");
        if (opt->value == 0) {
            opt = config.option<ConfigOptionFloatOrPercent>("perimeter_extrusion_width");
            if (opt == nullptr)
                throw_on_missing_variable(opt_key, "perimeter_extrusion_width");
        }
    }

    if (opt == nullptr)
        throw_on_missing_variable(opt_key, opt_key.c_str());

    // This is the logic used for skit / brim, but not for the rest of the 1st layer.
    if (opt->value == 0. && boost::starts_with(opt_key, "skirt")) {
        // The "skirt_extrusion_width" was set to zero, try a substitute.
        const ConfigOptionFloatOrPercent* opt_first_layer_extrusion_width = config.option<ConfigOptionFloatOrPercent>("first_layer_extrusion_width");
        const ConfigOptionInt* opt_skirt_height = config.option<ConfigOptionInt>("skirt_height");
        const ConfigOptionEnum<DraftShield>* opt_draft_shield = config.option<ConfigOptionEnum<DraftShield>>("draft_shield");
        if (opt_first_layer_extrusion_width == nullptr)
            throw_on_missing_variable(opt_key, "first_layer_extrusion_width");
        if (opt_draft_shield == nullptr)
            throw_on_missing_variable(opt_key, "draft_shield");
        if (opt_skirt_height == nullptr)
            throw_on_missing_variable(opt_key, "skirt_height");
        // The "first_layer_extrusion_width" was set to zero, try a substitute.
        if (opt_first_layer_extrusion_width && opt_draft_shield && opt_skirt_height && opt_first_layer_extrusion_width->value > 0 && opt_skirt_height->value == 1 && opt_draft_shield->value != DraftShield::dsDisabled)
            opt = opt_first_layer_extrusion_width;

        if (opt->value == 0) {
            opt = config.option<ConfigOptionFloatOrPercent>("perimeter_extrusion_width");
            if (opt == nullptr)
                throw_on_missing_variable(opt_key, "perimeter_extrusion_width");
        }
    }

    // external_perimeter_extrusion_width default is perimeter_extrusion_width
    //if (opt->value == 0. && boost::starts_with(opt_key, "external_perimeter_extrusion_width")) {
    //    // The role specific extrusion width value was set to zero, try the role non-specific extrusion width.
    //    opt = config.option<ConfigOptionFloatOrPercent>("perimeter_extrusion_width");
    //    if (opt == nullptr)
    //        throw_on_missing_variable(opt_key, "perimeter_extrusion_width");
    //}

    // top_infill_extrusion_width default is solid_infill_extrusion_width
    //if (opt->value == 0. && boost::starts_with(opt_key, "top_infill_extrusion_width")) {
    //    // The role specific extrusion width value was set to zero, try the role non-specific extrusion width.
    //    opt = config.option<ConfigOptionFloatOrPercent>("solid_infill_extrusion_width");
    //    if (opt == nullptr)
    //        throw_on_missing_variable(opt_key, "solid_infill_extrusion_width");
    //}

    if (opt->value == 0.) {
        // The role specific extrusion width value was set to zero, try the role non-specific extrusion width.
        opt = config.option<ConfigOptionFloatOrPercent>("extrusion_width");
        if (opt == nullptr)
            throw_on_missing_variable(opt_key, "extrusion_width");
    }

    return opt;
}

//used to get brim & skirt extrusion config
const ConfigOptionFloatOrPercent* Flow::extrusion_spacing_option(std::string opt_key, const ConfigOptionResolver& config)
{
    std::string opt_key_width;
    if (boost::starts_with(opt_key, "skirt")) {
        //skirt have only width setting
        if (!boost::ends_with(opt_key, "_extrusion_width")) {
            opt_key += "_extrusion_width";
        }
        opt_key_width = opt_key;
    } else {//brim
        if (boost::ends_with(opt_key, "_extrusion_width")) {
            boost::replace_first(opt_key, "_width", "_spacing");
        }
        if (!boost::ends_with(opt_key, "_extrusion_spacing")) {
            opt_key_width = opt_key + "_extrusion_width";
            opt_key += "_extrusion_spacing";
        } else {
            opt_key_width = opt_key;
            boost::replace_first(opt_key_width, "_spacing", "_width");
        }
    }

    const ConfigOptionFloatOrPercent* opt = config.option<ConfigOptionFloatOrPercent>(opt_key);

    //brim is first_layer_extrusion_spacing then perimeter_extrusion_spacing
    if (!opt && boost::starts_with(opt_key, "brim_")) {
        const ConfigOptionFloatOrPercent* optTest = config.option<ConfigOptionFloatOrPercent>("first_layer_extrusion_spacing");
        opt = optTest;
        if (opt == nullptr)
            throw_on_missing_variable(opt_key, "first_layer_extrusion_spacing");
        if (opt->value == 0) {
            opt = config.option<ConfigOptionFloatOrPercent>("perimeter_extrusion_spacing");
            if (opt == nullptr)
                throw_on_missing_variable(opt_key, "perimeter_extrusion_spacing");
        }
    }

    if (opt == nullptr) {
        opt_key = opt_key_width;
        opt = config.option<ConfigOptionFloatOrPercent>(opt_key_width);
    }

    if (opt == nullptr)
        throw_on_missing_variable(opt_key, opt_key.c_str());

    // This is the logic used for skit / brim, but not for the rest of the 1st layer.
    if (opt->value == 0. && boost::starts_with(opt_key, "skirt")) {
        // The "skirt_extrusion_spacing" was set to zero, try a substitute.
        const ConfigOptionFloatOrPercent* opt_first_layer_extrusion_spacing = config.option<ConfigOptionFloatOrPercent>("first_layer_extrusion_spacing");
        const ConfigOptionInt* opt_skirt_height = config.option<ConfigOptionInt>("skirt_height");
        const ConfigOptionEnum<DraftShield>* opt_draft_shield = config.option<ConfigOptionEnum<DraftShield>>("draft_shield");
        if (opt_first_layer_extrusion_spacing == nullptr)
            throw_on_missing_variable(opt_key, "first_layer_extrusion_spacing");
        if (opt_draft_shield == nullptr)
            throw_on_missing_variable(opt_key, "draft_shield");
        if (opt_skirt_height == nullptr)
            throw_on_missing_variable(opt_key, "skirt_height");
        // The "first_layer_extrusion_spacing" was set to zero, try a substitute.
        if (opt_first_layer_extrusion_spacing && opt_draft_shield && opt_skirt_height && opt_first_layer_extrusion_spacing->value > 0 && opt_skirt_height->value == 1 && opt_draft_shield->value != DraftShield::dsDisabled)
            opt = opt_first_layer_extrusion_spacing;

        if (opt->value == 0) {
            opt = config.option<ConfigOptionFloatOrPercent>("perimeter_extrusion_spacing");
            if (opt == nullptr)
                throw_on_missing_variable(opt_key, "perimeter_extrusion_spacing");
        }
    }

    // external_perimeter_extrusion_spacing default is perimeter_extrusion_spacing
    //if (opt->value == 0. && boost::starts_with(opt_key, "external_perimeter_extrusion_spacing")) {
    //    // The role specific extrusion width value was set to zero, try the role non-specific extrusion width.
    //    opt = config.option<ConfigOptionFloatOrPercent>("perimeter_extrusion_spacing");
    //    if (opt == nullptr)
    //        throw_on_missing_variable(opt_key, "perimeter_extrusion_spacing");
    //}

    // top_infill_extrusion_spacing default is solid_infill_extrusion_spacing
    //if (opt->value == 0. && boost::starts_with(opt_key, "top_infill_extrusion_spacing")) {
    //    // The role specific extrusion width value was set to zero, try the role non-specific extrusion width.
    //    opt = config.option<ConfigOptionFloatOrPercent>("solid_infill_extrusion_spacing");
    //    if (opt == nullptr)
    //        throw_on_missing_variable(opt_key, "solid_infill_extrusion_spacing");
    //}

    if (opt->value == 0.) {
        // The role specific extrusion width value was set to zero, try the role non-specific extrusion width.
        opt = config.option<ConfigOptionFloatOrPercent>("extrusion_spacing");
        if (opt == nullptr)
            throw_on_missing_variable(opt_key, "extrusion_spacing");
    }

    return opt;
}

// Used to provide hints to the user on default extrusion width values, and to provide reasonable values to the PlaceholderParser.
double Flow::extrusion_width(const std::string& opt_key, const ConfigOptionResolver &config, const unsigned int first_printing_extruder)
{
    return extrusion_width(opt_key, config.option<ConfigOptionFloatOrPercent>(opt_key), config, first_printing_extruder);
}

Flow Flow::new_from_config(FlowRole role, const DynamicConfig& print_config, float nozzle_diameter, float layer_height, float filament_max_overlap, bool first_layer) {

    ConfigOptionFloatOrPercent  config_width;
    ConfigOptionFloatOrPercent  config_spacing;
    // Get extrusion width from configuration.
    float overlap = 1.f;
    // (might be an absolute value, or a percent value, or zero for auto)
    if (role == frExternalPerimeter) {
        config_width = print_config.opt<ConfigOptionFloatOrPercent>("external_perimeter_extrusion_width");
        config_spacing = print_config.opt<ConfigOptionFloatOrPercent>("external_perimeter_extrusion_spacing");
        overlap = (float)print_config.get_abs_value("external_perimeter_overlap", 1.0);
    } else if (role == frPerimeter) {
        config_width = print_config.opt<ConfigOptionFloatOrPercent>("perimeter_extrusion_width");
        config_spacing = print_config.opt<ConfigOptionFloatOrPercent>("perimeter_extrusion_spacing");
        overlap = (float)print_config.get_abs_value("perimeter_overlap", 1.);
    } else if (role == frInfill) {
        config_width = print_config.opt<ConfigOptionFloatOrPercent>("infill_extrusion_width");
        config_spacing = print_config.opt<ConfigOptionFloatOrPercent>("infill_extrusion_spacing");
    } else if (role == frSolidInfill) {
        config_width = print_config.opt<ConfigOptionFloatOrPercent>("solid_infill_extrusion_width");
        config_spacing = print_config.opt<ConfigOptionFloatOrPercent>("solid_infill_extrusion_spacing");
        overlap = (float)print_config.get_abs_value("solid_infill_overlap", 1.);
    } else if (role == frTopSolidInfill) {
        config_width = print_config.opt<ConfigOptionFloatOrPercent>("top_infill_extrusion_width");
        config_spacing = print_config.opt<ConfigOptionFloatOrPercent>("top_infill_extrusion_spacing");
        overlap = (float)print_config.get_abs_value("solid_infill_overlap", 1.);
    } else {
        throw Slic3r::InvalidArgument("Unknown role");
    }
    if (first_layer && print_config.get_abs_value("first_layer_extrusion_width", 1) > 0) {
        config_width = print_config.opt<ConfigOptionFloatOrPercent>("first_layer_extrusion_width");
        config_spacing = print_config.opt<ConfigOptionFloatOrPercent>("first_layer_extrusion_spacing");
    }

    if (config_width.value == 0) {
        config_width = print_config.opt<ConfigOptionFloatOrPercent>("extrusion_width");
        config_spacing = print_config.opt<ConfigOptionFloatOrPercent>("extrusion_spacing");
    }

    // Get the configured nozzle_diameter for the extruder associated to the flow role requested.
    // Here this->extruder(role) - 1 may underflow to MAX_INT, but then the get_at() will follback to zero'th element, so everything is all right.
    return Flow::new_from_config_width(role, config_width, config_spacing, nozzle_diameter, layer_height, std::min(overlap, filament_max_overlap));
    //bridge ? (float)m_config.bridge_flow_ratio.get_abs_value(1) : 0.0f);
}

// This constructor builds a Flow object from an extrusion width config setting
// and other context properties.
Flow Flow::new_from_config_width(FlowRole role, const ConfigOptionFloatOrPercent& width, const ConfigOptionFloatOrPercent& spacing, float nozzle_diameter, float height, float spacing_ratio, float bridge_flow_ratio)
{
    if (height <= 0)
        throw Slic3r::InvalidArgument("Invalid flow height supplied to new_from_config_width()");

    float w = 0.f;
    if (bridge_flow_ratio > 0) {
        // If bridge flow was requested, calculate the bridge width.
        height = w = (bridge_flow_ratio == 1.) ?
            // optimization to avoid sqrt()
            nozzle_diameter :
            sqrt(bridge_flow_ratio) * nozzle_diameter;
    } else {
        if (!width.is_phony()) {
            if (!width.percent && width.value <= 0.) {
                // If user left option to 0, calculate a sane default width.
                w = auto_extrusion_width(role, nozzle_diameter);
            } else {
                // If user set a manual value, use it.
                w = float(width.get_abs_value(nozzle_diameter));
            }
        } else {
            if (!spacing.percent && spacing.value == 0.) {
                // If user left option to 0, calculate a sane default width.
                w = auto_extrusion_width(role, nozzle_diameter);
            } else {
                // If user set a manual value, use it.
                return new_from_spacing(float(spacing.get_abs_value(nozzle_diameter)), nozzle_diameter, height, spacing_ratio, false);
            }
        }
    }

    
    return Flow(w, height, rounded_rectangle_extrusion_spacing(w, height, spacing_ratio), nozzle_diameter, spacing_ratio, bridge_flow_ratio > 0);
}

// This constructor builds a Flow object from an extrusion width config setting
// and other context properties.
Flow Flow::new_from_config_width(FlowRole role, const ConfigOptionFloatOrPercent &width, const ConfigOptionFloatOrPercent& spacing, float nozzle_diameter, float height, float spacing_ratio)
{
    if (height <= 0)
        throw Slic3r::InvalidArgument("Invalid flow height supplied to new_from_config_width()");

    float w;
    if (!width.is_phony()) {
        if (!width.percent && width.value == 0.) {
            // If user left option to 0, calculate a sane default width.
            w = auto_extrusion_width(role, nozzle_diameter);
        } else {
            // If user set a manual value, use it.
            w = float(width.get_abs_value(nozzle_diameter));
        }
    } else {
        if (!spacing.percent && spacing.value == 0.) {
            // If user left option to 0, calculate a sane default width.
            w = auto_extrusion_width(role, nozzle_diameter);
        } else {
            // If user set a manual value, use it.
            return new_from_spacing(float(spacing.get_abs_value(nozzle_diameter)), nozzle_diameter, height, spacing_ratio, false);
        }
    }

    return Flow(w, height, rounded_rectangle_extrusion_spacing(w, height, spacing_ratio), nozzle_diameter, spacing_ratio, false);
}

// This constructor builds a Flow object from a given centerline spacing.
Flow Flow::new_from_spacing(float spacing, float nozzle_diameter, float height, float spacing_ratio, bool bridge)
{
    // we need layer height unless it's a bridge
    if (height <= 0 && !bridge)
        throw Slic3r::InvalidArgument("Invalid flow height supplied to new_from_spacing()");
    // Calculate width from spacing.
    // For normal extrusons, extrusion width is wider than the spacing due to the rounding and squishing of the extrusions.
    // For bridge extrusions, the extrusions are placed with a tiny BRIDGE_EXTRA_SPACING gaps between the threads.
    float width = float(bridge ?
        (spacing /*- BRIDGE_EXTRA_SPACING_MULT (0.125) * nozzle_diameter*/) :
        rounded_rectangle_extrusion_width_from_spacing(spacing, height, spacing_ratio));
    return Flow(width, bridge ? width : height, spacing, nozzle_diameter, bridge ? 0 : spacing_ratio, bridge);
}

// This constructor builds a Flow object from a given centerline spacing.
Flow Flow::new_from_width(float width, float nozzle_diameter, float height, float spacing_ratio, bool bridge)
{
    // we need layer height unless it's a bridge
    if (height <= 0 && !bridge)
        throw Slic3r::InvalidArgument("Invalid flow height supplied to new_from_width()");

    float spacing = float(bridge ?
        (width /*- BRIDGE_EXTRA_SPACING_MULT (0.125) * nozzle_diameter*/) :
        rounded_rectangle_extrusion_spacing(width, height, spacing_ratio));
    return Flow(width, bridge ? width : height, spacing, nozzle_diameter, bridge ? 0 : spacing_ratio, bridge);
}


// Adjust extrusion flow for new extrusion line spacing, maintaining the old spacing between extrusions.
Flow Flow::with_spacing(float new_spacing) const
{
    Flow out = *this;
    if (m_bridge) {
        // Diameter of the rounded extrusion.
        assert(m_width == m_height);
        float gap          = m_spacing - m_width;
        auto  new_diameter = new_spacing - gap;
        out.m_width        = out.m_height = new_diameter;
    } else {
        assert(m_width >= m_height);
        out.m_width += new_spacing - m_spacing;
        if (out.m_width < out.m_height)
            throw Slic3r::InvalidArgument("Invalid spacing supplied to Flow::with_spacing()");
    }
    out.m_spacing = new_spacing;
    return out;
}

Flow Flow::with_spacing_ratio_from_width(float new_spacing_ratio) const
{
    return Flow::new_from_width(m_width, m_nozzle_diameter, m_height, new_spacing_ratio, m_bridge);
}

// This method returns the centerline spacing between two adjacent extrusions 
// having the same extrusion width (and other properties).
float Flow::spacing() const 
{
#ifdef HAS_PERIMETER_LINE_OVERLAP
    if (this->bridge)
        return this->width + BRIDGE_EXTRA_SPACING;
    // rectangle with semicircles at the ends
    float min_flow_spacing = this->width - this->height * (1. - 0.25 * PI) * spacing_ratio;
    float res = this->width - PERIMETER_LINE_OVERLAP_FACTOR * (this->width - min_flow_spacing);
#else
    float res = float(this->bridge() ? (this->width() /*+ BRIDGE_EXTRA_SPACING_MULT * nozzle_diameter*/) : (this->width() - this->height() * (1. - 0.25 * PI) * m_spacing_ratio));
#endif
//    assert(res > 0.f);
	if (res <= 0.f)
		throw FlowErrorNegativeSpacing();
	return res;
}

// Adjust the width / height of a rounded extrusion model to reach the prescribed cross section area while maintaining extrusion spacing.
Flow Flow::with_cross_section(float area_new) const
{
    assert(! m_bridge);
    assert(m_width >= m_height);

    // Adjust for bridge_flow_ratio, maintain the extrusion spacing.
    float area = this->mm3_per_mm();
    if (area_new > area + EPSILON) {
        // Increasing the flow rate.
        float new_full_spacing = area_new / m_height;
        if (new_full_spacing > m_spacing) {
            // Filling up the spacing without an air gap. Grow the extrusion in height.
            float height = area_new / m_spacing;
            return Flow(rounded_rectangle_extrusion_width_from_spacing(m_spacing, height, m_spacing_ratio), height, m_spacing, m_nozzle_diameter, m_spacing_ratio, false);
        } else {
            return this->with_width(rounded_rectangle_extrusion_width_from_spacing(area / m_height, m_height, m_spacing_ratio));
        }
    } else if (area_new < area - EPSILON) {
        // Decreasing the flow rate.
        float width_new = m_width - (area - area_new) / m_height;
        assert(width_new > 0);
        if (width_new > m_height) {
            // Shrink the extrusion width.
            return this->with_width(width_new);
        } else {
            // Create a rounded extrusion.
            auto dmr = float(sqrt(area_new / M_PI));
            return Flow(dmr, dmr, m_spacing, m_nozzle_diameter, m_spacing_ratio, false);
        }
    } else
        return *this;
}

// This method returns the centerline spacing between an extrusion using this
// flow and another one using another flow.
// this->spacing(other) shall return the same value as other.spacing(*this)
float Flow::spacing(const Flow &other) const
{
    assert(this->height() == other.height());
    assert(this->bridge() == other.bridge());
    float res = float(this->bridge() || other.bridge() ?
        0.5 * this->width() + 0.5 * other.width() :
        0.5 * this->spacing() + 0.5 * other.spacing());
//    assert(res > 0.f);
	if (res <= 0.f)
		throw FlowErrorNegativeSpacing();
	return res;
}

float Flow::rounded_rectangle_extrusion_spacing(float width, float height, float m_spacing_ratio)
{
#ifdef HAS_PERIMETER_LINE_OVERLAP
    return (spacing - PERIMETER_LINE_OVERLAP_FACTOR * height * (1. - 0.25 * PI) * spacing_ratio);
#else
    if (width == height && width == 0)
        return 0.f;
    float out = width - height * float(1. - 0.25 * PI) * m_spacing_ratio;
    if (out <= 0.f)
        throw FlowErrorNegativeSpacing();
    return out;
#endif
}

float Flow::rounded_rectangle_extrusion_width_from_spacing(float spacing, float height, float m_spacing_ratio)
{
#ifdef HAS_PERIMETER_LINE_OVERLAP
    return (spacing + PERIMETER_LINE_OVERLAP_FACTOR * height * (1. - 0.25 * PI) * spacing_ratio);
#else
    return float(spacing + height * (1. - 0.25 * PI) * m_spacing_ratio);
#endif
}

float Flow::bridge_extrusion_spacing(float dmr)
{
    return dmr;// +BRIDGE_EXTRA_SPACING;
}

// This method returns extrusion volume per head move unit.
double Flow::mm3_per_mm() const
{
    float res = m_bridge ?
        // Area of a circle with dmr of this->width.
        float((m_width * m_width) * 0.25 * PI) :
        // Rectangle with semicircles at the ends. ~ h (w - 0.215 h)
        float(m_height * (m_width - m_height * (1. - 0.25 * PI)));
    //assert(res > 0.);
	if (res <= 0.)
		throw FlowErrorNegativeFlow();
    return res;
}

Flow support_material_flow(const PrintObject* object, float layer_height)
{
    int extruder_id = object->config().support_material_extruder.value - 1;
    if (extruder_id < 0) {
        extruder_id = object->layers().front()->get_region(0)->region().config().perimeter_extruder - 1;
    }
    double nzd = object->print()->config().nozzle_diameter.get_at(extruder_id);
    const ConfigOptionFloatOrPercent& width = (object->config().support_material_extrusion_width.value > 0) ? object->config().support_material_extrusion_width : object->config().extrusion_width;
    const ConfigOptionFloatOrPercent& spacing = (object->config().support_material_extrusion_width.value > 0) ? object->config().support_material_extrusion_width : object->config().extrusion_spacing;
    float max_height = 0.f;
    if (!width.percent && width.value <= 0.) {
        // If user left option to 0, calculate a sane default width.
        max_height = Flow::auto_extrusion_width(frSupportMaterialInterface, nzd);
    } else {
        // If user set a manual value, use it.
        max_height = float(width.get_abs_value(nzd));
    }
    if (layer_height <= 0) { // get default layer height for material interface
        layer_height = object->config().support_material_layer_height.get_abs_value(nzd);
        if (layer_height == 0) {
            layer_height = object->print()->config().max_layer_height.get_abs_value(extruder_id, nzd);
            if (layer_height == 0) {
                layer_height = nzd * 0.75;
            }
        }
    }
    layer_height = std::min(layer_height, max_height);
    return Flow::new_from_config_width(
        frSupportMaterial,
        // The width parameter accepted by new_from_config_width is of type ConfigOptionFloatOrPercent, the Flow class takes care of the percent to value substitution.
        width,
        spacing,
        // if object->config().support_material_extruder == 0 (which means to not trigger tool change, but use the current extruder instead), get_at will return the 0th component.
        float(nzd),
        layer_height,
        extruder_id < 0 ? 1 : object->config().get_computed_value("filament_max_overlap", extruder_id), //if can get an extruder, then use its param, or use full overlap if we don't know the extruder id.
        // bridge_flow_ratio
        0.f);
}

Flow support_material_1st_layer_flow(const PrintObject *object, float layer_height)
{

    const PrintConfig &print_config = object->print()->config();
    const auto& width = (object->config().first_layer_extrusion_width.value > 0) ? object->config().first_layer_extrusion_width : object->config().support_material_extrusion_width;
    const auto& spacing = (object->config().first_layer_extrusion_spacing.value > 0) ? object->config().first_layer_extrusion_spacing : object->config().support_material_extrusion_width;
    float slice_height = layer_height;
    if (layer_height <= 0.f && !object->print()->config().nozzle_diameter.empty()){
        slice_height = (float)object->get_first_layer_height();
    }
    int extruder_id = object->config().support_material_extruder.value -1;
    if (extruder_id < 0) {
        extruder_id = object->layers().front()->get_region(0)->region().config().infill_extruder - 1;
    }
    return Flow::new_from_config_width(
        frSupportMaterial,
        // The width parameter accepted by new_from_config_width is of type ConfigOptionFloatOrPercent, the Flow class takes care of the percent to value substitution.
        (width.value > 0) ? width : object->config().extrusion_width,
        spacing, // can be used if first_layer_extrusion_width is phony
        float(print_config.nozzle_diameter.get_at(extruder_id)),
        slice_height,
        extruder_id < 0 ? 1 : object->config().get_computed_value("filament_max_overlap", extruder_id), //if can get an extruder, then use its param, or use full overlap if we don't know the extruder id.
        // bridge_flow_ratio
        0.f);
}

Flow support_material_interface_flow(const PrintObject* object, float layer_height)
{
    int extruder_id = object->config().support_material_interface_extruder.value - 1;
    if (extruder_id < 0) {
        extruder_id = object->layers().front()->get_region(0)->region().config().infill_extruder - 1;
    }
    double nzd = object->print()->config().nozzle_diameter.get_at(extruder_id);
    const ConfigOptionFloatOrPercent& width = (object->config().support_material_extrusion_width.value > 0) ? object->config().support_material_extrusion_width : object->config().extrusion_width;
    const ConfigOptionFloatOrPercent& spacing = (object->config().support_material_extrusion_width.value > 0) ? object->config().support_material_extrusion_width : object->config().extrusion_spacing;
    float max_height = 0.f;
    if (!width.percent && width.value <= 0.) {
        // If user left option to 0, calculate a sane default width.
        max_height = Flow::auto_extrusion_width(frSupportMaterialInterface, nzd);
    } else {
        // If user set a manual value, use it.
        max_height = float(width.get_abs_value(nzd));
    }
    if (layer_height <= 0) { // get default layer height for material interface
        layer_height = object->config().support_material_interface_layer_height.get_abs_value(nzd);
        if (layer_height == 0) {
            layer_height = object->print()->config().max_layer_height.get_abs_value(extruder_id, nzd);
            if (layer_height == 0) {
                layer_height = nzd * 0.75;
            }
        }
    }
    layer_height = std::min(layer_height, max_height);
    return Flow::new_from_config_width(
        frSupportMaterialInterface,
        // The width parameter accepted by new_from_config_width is of type ConfigOptionFloatOrPercent, the Flow class takes care of the percent to value substitution.
        width,
        spacing,
        // if object->config().support_material_interface_extruder == 0 (which means to not trigger tool change, but use the current extruder instead), get_at will return the 0th component.
        float(nzd),
        layer_height,
        extruder_id < 0 ? 1 : object->config().get_computed_value("filament_max_overlap", extruder_id), //if can get an extruder, then use its param, or use full overlap if we don't know the extruder id.
        // bridge_flow_ratio
        0.f);
}

Flow raft_flow(const PrintObject* object, float layer_height)
{
    int extruder_id = object->config().support_material_interface_extruder.value - 1;
    if (extruder_id < 0) {
        extruder_id = object->layers().front()->get_region(0)->region().config().perimeter_extruder - 1;
    }
    double nzd = object->print()->config().nozzle_diameter.get_at(extruder_id);
    const ConfigOptionFloatOrPercent& width = (object->config().support_material_extrusion_width.value > 0) ? object->config().support_material_extrusion_width : object->config().extrusion_width;
    const ConfigOptionFloatOrPercent& spacing = (object->config().support_material_extrusion_width.value > 0) ? object->config().support_material_extrusion_width : object->config().extrusion_spacing;
    float max_height = 0.f;
    if (!width.percent && width.value <= 0.) {
        // If user left option to 0, calculate a sane default width.
        max_height = Flow::auto_extrusion_width(frSupportMaterial, nzd);
    } else {
        // If user set a manual value, use it.
        max_height = float(width.get_abs_value(nzd));
    }
    if (layer_height <= 0) { // get default layer height for material interface
        layer_height = object->config().raft_interface_layer_height.get_abs_value(nzd);
        if (layer_height == 0) {
            layer_height = object->print()->config().max_layer_height.get_abs_value(extruder_id, nzd);
            if (layer_height == 0) {
                layer_height = nzd * 0.75;
            }
        }
    }
    layer_height = std::min(layer_height, max_height);
    return Flow::new_from_config_width(
        frSupportMaterial,
        // The width parameter accepted by new_from_config_width is of type ConfigOptionFloatOrPercent, the Flow class takes care of the percent to value substitution.
        width,
        spacing,
        // if object->config().support_material_interface_extruder == 0 (which means to not trigger tool change, but use the current extruder instead), get_at will return the 0th component.
        float(nzd),
        layer_height,
        extruder_id < 0 ? 1 : object->config().get_computed_value("filament_max_overlap", extruder_id), //if can get an extruder, then use its param, or use full overlap if we don't know the extruder id.
        // bridge_flow_ratio
        0.f);
}

Flow raft_interface_flow(const PrintObject* object, float layer_height)
{
    int extruder_id = object->config().support_material_interface_extruder.value - 1;
    if (extruder_id < 0) {
        extruder_id = object->layers().front()->get_region(0)->region().config().infill_extruder - 1;
    }
    double nzd = object->print()->config().nozzle_diameter.get_at(extruder_id);
    const ConfigOptionFloatOrPercent& width = (object->config().support_material_extrusion_width.value > 0) ? object->config().support_material_extrusion_width : object->config().extrusion_width;
    const ConfigOptionFloatOrPercent& spacing = (object->config().support_material_extrusion_width.value > 0) ? object->config().support_material_extrusion_width : object->config().extrusion_spacing;
    float max_height = 0.f;
    if (!width.percent && width.value <= 0.) {
        // If user left option to 0, calculate a sane default width.
        max_height = Flow::auto_extrusion_width(frSupportMaterialInterface, nzd);
    } else {
        // If user set a manual value, use it.
        max_height = float(width.get_abs_value(nzd));
    }
    if (layer_height <= 0) { // get default layer height for material interface
        layer_height = object->config().raft_interface_layer_height.get_abs_value(nzd);
        if (layer_height == 0) {
            layer_height = object->print()->config().max_layer_height.get_abs_value(extruder_id, nzd);
            if (layer_height == 0) {
                layer_height = nzd * 0.75;
            }
        }
    }
    layer_height = std::min(layer_height, max_height);
    return Flow::new_from_config_width(
        frSupportMaterialInterface,
        // The width parameter accepted by new_from_config_width is of type ConfigOptionFloatOrPercent, the Flow class takes care of the percent to value substitution.
        width,
        spacing,
        // if object->config().support_material_interface_extruder == 0 (which means to not trigger tool change, but use the current extruder instead), get_at will return the 0th component.
        float(nzd),
        layer_height,
        extruder_id < 0 ? 1 : object->config().get_computed_value("filament_max_overlap", extruder_id), //if can get an extruder, then use its param, or use full overlap if we don't know the extruder id.
        // bridge_flow_ratio
        0.f);
}

}
