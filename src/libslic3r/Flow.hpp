#ifndef slic3r_Flow_hpp_
#define slic3r_Flow_hpp_

#include "libslic3r.h"
#include "Config.hpp"
#include "Exception.hpp"
#include "ExtrusionEntity.hpp"

namespace Slic3r {

class PrintObject;

enum FlowRole {
    frExternalPerimeter,
    frPerimeter,
    frInfill,
    frSolidInfill,
    frTopSolidInfill,
    frSupportMaterial,
    frSupportMaterialInterface,
};

class FlowError : public Slic3r::InvalidArgument
{
public:
    FlowError(const std::string& what_arg) : Slic3r::InvalidArgument(what_arg) {}
    FlowError(const char* what_arg) : Slic3r::InvalidArgument(what_arg) {}
};

class FlowErrorNegativeSpacing : public FlowError
{
public:
    FlowErrorNegativeSpacing();
};

class FlowErrorNegativeFlow : public FlowError
{
public:
    FlowErrorNegativeFlow();
};

class FlowErrorMissingVariable : public FlowError
{
public:
    FlowErrorMissingVariable(const std::string& what_arg) : FlowError(what_arg) {}
};

class Flow
{
private:
    Flow(float width, float height, float nozzle_diameter, float spacing_ratio) :
        m_width(width), m_height(height), m_spacing(rounded_rectangle_extrusion_spacing(width, height, spacing_ratio)), m_nozzle_diameter(nozzle_diameter), m_spacing_ratio(spacing_ratio), m_bridge(false) {}
    //Flow(width, height, rounded_rectangle_extrusion_spacing(width, height, spacing_ratio), nozzle_diameter, spacing_ratio, false) {}
    Flow(float width, float height, float spacing, float _nd, float spacing_ratio, bool _bridge) :
        m_width(width), m_height(height), m_spacing(spacing), m_nozzle_diameter(_nd), m_spacing_ratio(spacing_ratio), m_bridge(_bridge) {}
public:
    Flow() = default;

    // Non bridging flow: Maximum width of an extrusion with semicircles at the ends.
    // Bridging flow: Bridge thread diameter.
    float   width()           const { return m_width; }
    coord_t scaled_width()    const { return scale_t(m_width); }
    // Non bridging flow: Layer height.
    // Bridging flow: Bridge thread diameter = layer height.
    float   height()          const { return m_height; }
    // Spacing between the extrusion centerlines.
    float   spacing()         const;// { return m_spacing; } use the compute, as I can't be 100% sure yet that this cachced value is good.
    coord_t scaled_spacing()  const { return scale_t(spacing()); }
    float   spacing_ratio()   const { return m_spacing_ratio; }
    // Nozzle diameter.
    float   nozzle_diameter() const { return m_nozzle_diameter; }
    // Is it a bridge?
    bool    bridge()          const { return m_bridge; }
    // Cross section area of the extrusion.
    double  mm3_per_mm()      const;

    float overlap(float height) const {
        return (float)(height * (1. - 0.25 * PI)) * m_spacing_ratio;
    }
    // is it still needed?
    float spacing(const Flow& other) const;
    coord_t scaled_spacing(const Flow& other) const { return scale_t(this->spacing(other)); }

    // Elephant foot compensation spacing to be used to detect narrow parts, where the elephant foot compensation cannot be applied.
    // To be used on frExternalPerimeter only.
    // Enable some perimeter squish (see INSET_OVERLAP_TOLERANCE).
    // Here an overlap of 0.2x external perimeter spacing is allowed for by the elephant foot compensation.
    coord_t scaled_elephant_foot_spacing() const { return coord_t(0.5f * float(this->scaled_width() + 0.6f * this->scaled_spacing())); }

    bool operator==(const Flow &rhs) const {
        return m_width == rhs.m_width
            && m_height == rhs.m_height
            && m_nozzle_diameter == rhs.m_nozzle_diameter
            && m_spacing_ratio == rhs.m_spacing_ratio
            && m_bridge == rhs.m_bridge; }

    Flow        with_width (float width)  const {
        assert(! m_bridge);
        return Flow(width, m_height, rounded_rectangle_extrusion_spacing(width, m_height, m_spacing_ratio), m_nozzle_diameter, m_spacing_ratio, m_bridge);
    }
    Flow        with_height(float height) const {
        assert(! m_bridge);
        return Flow(m_width, height, rounded_rectangle_extrusion_spacing(m_width, height, m_spacing_ratio), m_nozzle_diameter, m_spacing_ratio, m_bridge);
    }
    // Adjust extrusion flow for new extrusion line spacing, maintaining the old spacing between extrusions.
    Flow        with_spacing(float spacing) const;
    Flow        with_spacing_ratio_from_width(float spacing_ratio) const;
    // Adjust the width / height of a rounded extrusion model to reach the prescribed cross section area while maintaining extrusion spacing.
    Flow        with_cross_section(float area) const;
    Flow        with_flow_ratio(double ratio) const { return this->with_cross_section(this->mm3_per_mm() * ratio); }

    // Spacing of extrusions with rounded extrusion model.
    static float rounded_rectangle_extrusion_spacing(float width, float height, float spacing_ratio);
    // Width of extrusions with rounded extrusion model.
    static float rounded_rectangle_extrusion_width_from_spacing(float spacing, float height, float spacing_ratio);
    // Spacing of round thread extrusions.
    static float bridge_extrusion_spacing(float dmr);

    // Sane extrusion width default based on nozzle diameter.
    // The defaults were derived from manual Prusa MK3 profiles.
    static float auto_extrusion_width(FlowRole role, float nozzle_diameter);

    // Extrusion width from full config, taking into account the defaults (when set to zero) and ratios (percentages).
    // Precise value depends on layer index (1st layer vs. other layers vs. variable layer height),
    // on active extruder etc. Therefore the value calculated by this function shall be used as a hint only.
    static double extrusion_width(const std::string &opt_key, const ConfigOptionFloatOrPercent *opt, const ConfigOptionResolver &config, const unsigned int first_printing_extruder = 0);
    static double extrusion_width(const std::string &opt_key, const ConfigOptionResolver &config, const unsigned int first_printing_extruder = 0);
    static const ConfigOptionFloatOrPercent* extrusion_width_option(std::string opt_key, const ConfigOptionResolver& config);
    static const ConfigOptionFloatOrPercent* extrusion_spacing_option(std::string opt_key, const ConfigOptionResolver& config);

    // like PrintRegion::flow() but with print settings from a DynamicConfig
    static Flow new_from_config(FlowRole role, const DynamicConfig& print_config, float nozzle_diameter, float layer_height, float filament_max_overlap, bool first_layer);

    //low level constructor
    static Flow new_from_config_width(FlowRole role, const ConfigOptionFloatOrPercent& width, const ConfigOptionFloatOrPercent& spacing, float nozzle_diameter, float height, float spacing_ratio);
    static Flow bridging_flow(float dmr, float nozzle_diameter) { return Flow{ dmr, dmr, bridge_extrusion_spacing(dmr), nozzle_diameter, 0, true }; }
    // old constructors don't use them. They are used by the good constructors from PrintRegion or the default_flow().
    static Flow new_from_config_width(FlowRole role, const ConfigOptionFloatOrPercent& width, const ConfigOptionFloatOrPercent& spacing, float nozzle_diameter, float height, float spacing_ratio, float bridge_flow_ratio);
    // Create a flow from the spacing of extrusion lines.
    // This method is used exclusively to calculate new flow of 100% infill, where the extrusion width was allowed to scale
    // to fit a region with integer number of lines.
    static Flow new_from_spacing(float spacing, float nozzle_diameter, float height, float spacing_ratio, bool bridge = false);
    static Flow new_from_width(float width, float nozzle_diameter, float height, float spacing_ratio, bool bridge = false);


private:
    //Flow(float width, float height, float spacing, float nozzle_diameter, bool bridge) :
    //    m_width(width), m_height(height), m_spacing(spacing), m_nozzle_diameter(nozzle_diameter), m_bridge(bridge)
    //    {
    //        // Gap fill violates this condition.
    //        //assert(width >= height);
    //    }

    float       m_width { 0 };
    float       m_height { 0 };
    float       m_spacing { 0 };
    float       m_nozzle_diameter { 0 };
    bool        m_bridge { false };
    // % of spacing taken into account 1=>all/default, 0=> width=spacing
    float       m_spacing_ratio { 1 };
    //note: as there is a relation between {m_width, m_height, m_spacing, m_spacing_ratio} then one of these can be deleted.
};

extern Flow support_material_flow(const PrintObject *object, float layer_height = 0.f);
extern Flow support_material_1st_layer_flow(const PrintObject *object, float layer_height = 0.f);
extern Flow support_material_interface_flow(const PrintObject* object, float layer_height = 0.f);
extern Flow raft_flow(const PrintObject* object, float layer_height = 0.f);
extern Flow raft_interface_flow(const PrintObject* object, float layer_height = 0.f);

}

#endif
