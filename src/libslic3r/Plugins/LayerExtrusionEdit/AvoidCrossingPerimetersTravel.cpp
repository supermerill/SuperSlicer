///|/ Copyright (c) SuperSlicer 2026 Durand Remi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/

/*
Perimeter-avoiding travel generation
====================================

Each worker owns its routers and crossing caches while it processes one final
PrintingLayerGroup. Regional activation is resolved only at the two endpoints
of a gap. The direct segment is retained unless the optimized crossing test
confirms that it crosses printable boundaries.

All structural insertion, epsilon snapping and Z interpolation are delegated
to TravelConnectionHelpers, keeping this file focused on route selection.
*/

#include "AvoidCrossingPerimetersTravel.hpp"

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <map>
#include <memory>
#include <stdexcept>
#include <utility>
#include <vector>

#include "libslic3r/Api/plugin/c/steps/slic3r_step_layer_extrusion_edit.h"
#include "libslic3r/Api/plugin/cpp/PluginBase.hpp"
#include "libslic3r/Api/plugin/cpp/PrintingPlanViews.hpp"
#include "libslic3r/Api/plugin/cpp/RegionSettingsViews.hpp"
#include "libslic3r/GCode/AvoidCrossingPerimeters.hpp"
#include "libslic3r/Layer.hpp"
#include "libslic3r/Plugins/PrintingPlan/PrintingLayerEntryStateProperties.h"

#include "TravelConnectionHelpers.hpp"

namespace slic3r_api { namespace LayerExtrusionEdit { namespace AvoidCrossingPerimetersTravelPlugin {
namespace {

using namespace TravelConnection;

const char *const k_dependencies[] = {
    "layer_extrusion_edit.entry_state.default",
    nullptr
};

const char *const k_region_setting_keys[] = {
    "avoid_crossing_perimeters"
};

/* Identify one reusable router inside a layer-group worker. */
struct RouterKey
{
    const Slic3r::Layer *layer = nullptr;
    uint16_t extruder_id = uint16_t(-1);

    bool operator<(const RouterKey &rhs) const
    {
        return layer < rhs.layer || (layer == rhs.layer && extruder_id < rhs.extruder_id);
    }
};

/*
Own the expensive routing state used by one parallel layer-group run.

The planner never escapes run_impl(), so its map and the mutable caches inside
AvoidCrossingPerimeters require no synchronization.
*/
class LayerTravelPlanner
{
public:
    LayerTravelPlanner(const Print &print, const plugin_run_context *run_ctx);

    std::vector<c_point> path(const TravelEndpoint &source,
                              const TravelEndpoint &target,
                              uint16_t extruder_id);

private:
    /* Resolve one endpoint's regional setting, probing inside on a boundary. */
    bool endpoint_enables_avoidance(const TravelEndpoint &endpoint) const;

    /* Convert a final-plan point into the source object's local coordinates. */
    c_point local_point(c_point point, const TravelEndpoint &endpoint) const;

    /* Find or construct the router dedicated to one layer and extruder. */
    Slic3r::AvoidCrossingPerimeters &router_for(const Slic3r::Layer &layer,
                                                uint16_t extruder_id);

    /* Return a straight path whenever the advanced provider cannot be used. */
    std::vector<c_point> straight(const TravelEndpoint &source,
                                  const TravelEndpoint &target,
                                  uint16_t extruder_id) const;

    Config m_print_config;
    const plugin_run_context *m_run_ctx = nullptr;
    std::map<RouterKey, std::unique_ptr<Slic3r::AvoidCrossingPerimeters>> m_routers;
};

class AvoidCrossingPerimetersTravel : public PluginBase
{
public:
    static AvoidCrossingPerimetersTravel &instance(orchestrator_handle *orchestrator);
    explicit AvoidCrossingPerimetersTravel(orchestrator_handle *orchestrator);

private:
    const char *id_impl() const noexcept override;
    const char *name_impl() const noexcept override;
    const char *description_impl() const noexcept override;
    const char *exclusive_group_impl() const noexcept override;
    const char *exclusive_group_label_impl() const noexcept override;
    const char *exclusive_group_tooltip_impl() const noexcept override;
    slicing_step_t step_impl() const noexcept override;
    const char *const *dependencies_impl() const noexcept override;
    int32_t priority_impl() const noexcept override;
    int32_t used_config_keys(raw_used_config_key *keys) const noexcept override;
    const char *progress_message_format_impl() const noexcept override;
    void setup_impl(const plugin_run_context *run_ctx, uint32_t run_count) const override;
    void setup_run_impl(const plugin_run_context *run_ctx) const override;
    void run_impl(const plugin_run_context *run_ctx) const override;

    PluginPropertyKey<PrintingLayerEntryPositionProperty> m_entry_position_property;
    mutable bool m_setup_valid = false;
};

LayerTravelPlanner::LayerTravelPlanner(const Print &print, const plugin_run_context *run_ctx) :
    m_print_config(print.config()),
    m_run_ctx(run_ctx)
{}

c_point LayerTravelPlanner::local_point(const c_point point, const TravelEndpoint &endpoint) const
{
    const Layer source_layer = endpoint.region_island.region(0).layer();
    const Object source_object = source_layer.object();
    if (endpoint.object_instance_idx >= source_object.instance_count())
        throw std::runtime_error("A travel endpoint refers to an invalid object instance.");
    const c_point shift = source_object.instance_shift(endpoint.object_instance_idx);
    return c_point{point.x - shift.x, point.y - shift.y};
}

bool LayerTravelPlanner::endpoint_enables_avoidance(const TravelEndpoint &endpoint) const
{
    if (!endpoint.region_island.valid() || endpoint.region_island.region_count() == 0)
        return false;

    const c_point endpoint_point{endpoint.position.x, endpoint.position.y};
    RegionSettingsPointResult result = RegionSettings::lookup_at_point(
        endpoint.region_island, {k_region_setting_keys[0]}, local_point(endpoint_point, endpoint));
    if (result.found())
        return result.value.get_bool();

    // A point on a modifier boundary may belong to two regions. The probe was
    // sampled just inside the geometric leaf, so it identifies the side that
    // this endpoint actually enters or leaves.
    if (endpoint.has_interior_point) {
        result = RegionSettings::lookup_at_point(
            endpoint.region_island,
            {k_region_setting_keys[0]},
            local_point(endpoint.interior_point, endpoint));
        if (result.found())
            return result.value.get_bool();
    }
    return false;
}

Slic3r::AvoidCrossingPerimeters &LayerTravelPlanner::router_for(
    const Slic3r::Layer &layer,
    const uint16_t extruder_id)
{
    const RouterKey key{&layer, extruder_id};
    std::map<RouterKey, std::unique_ptr<Slic3r::AvoidCrossingPerimeters>>::iterator found =
        m_routers.find(key);
    if (found != m_routers.end())
        return *found->second;

    std::unique_ptr<Slic3r::AvoidCrossingPerimeters> router =
        std::make_unique<Slic3r::AvoidCrossingPerimeters>();
    router->init_layer(layer);
    std::pair<std::map<RouterKey, std::unique_ptr<Slic3r::AvoidCrossingPerimeters>>::iterator, bool> inserted =
        m_routers.emplace(key, std::move(router));
    return *inserted.first->second;
}

std::vector<c_point> LayerTravelPlanner::straight(const TravelEndpoint &source,
                                                   const TravelEndpoint &target,
                                                   const uint16_t extruder_id) const
{
    return straight_path(source, target, extruder_id);
}

std::vector<c_point> LayerTravelPlanner::path(const TravelEndpoint &source,
                                              const TravelEndpoint &target,
                                              const uint16_t extruder_id)
{
    // Missing regional provenance is normal for support and print-level
    // auxiliary paths. Those gaps deliberately retain the direct travel.
    if (!source.region_island.valid() || source.region_island.region_count() == 0 ||
        !target.region_island.valid() || target.region_island.region_count() == 0)
        return straight(source, target, extruder_id);
    if (!endpoint_enables_avoidance(source) || !endpoint_enables_avoidance(target))
        return straight(source, target, extruder_id);

    const Layer source_layer_view = source.region_island.region(0).layer();
    const Layer target_layer_view = target.region_island.region(0).layer();
    const Object source_object = source_layer_view.object();
    const Object target_object = target_layer_view.object();
    if (target.object_instance_idx >= target_object.instance_count())
        return straight(source, target, extruder_id);

    const Slic3r::Layer *target_layer =
        reinterpret_cast<const Slic3r::Layer *>(target_layer_view.handle());
    if (target_layer == nullptr)
        return straight(source, target, extruder_id);
    if (m_print_config.bool_or_default("avoid_crossing_not_first_layer", true) && target_layer->id() == 0)
        return straight(source, target, extruder_id);

    const c_point target_shift = target_object.instance_shift(target.object_instance_idx);
    const Slic3r::Point local_source{
        source.position.x - target_shift.x,
        source.position.y - target_shift.y};
    const Slic3r::Point local_target{
        target.position.x - target_shift.x,
        target.position.y - target_shift.y};

    Slic3r::AvoidCrossingPerimeters &router = router_for(*target_layer, extruder_id);
    const double nozzle_diameter =
        m_print_config.vector_float_or_default("nozzle_diameter", extruder_id, 0.4);
    const coord_t nozzle_radius = std::max<coord_t>(1, Slic3r::scale_i(nozzle_diameter * 0.5));
    const std::vector<const Slic3r::Layer *> printed_layers{target_layer};
    const Slic3r::AvoidCrossingPerimeters::PerimeterCrossingContext crossing_context{
        *target_layer,
        printed_layers,
        target.object_instance_idx,
        extruder_id,
        nozzle_radius,
        [this]() { throw_if_cancelled(m_run_ctx); }
    };
    router.prepare_crossing_test(crossing_context);

    Slic3r::Polyline direct;
    direct.points = {local_source, local_target};
    if (!router.can_cross_perimeter(direct, true))
        return straight(source, target, extruder_id);

    const bool same_object_and_instance =
        source_object.handle() == target_object.handle() &&
        source.object_instance_idx == target.object_instance_idx;
    router.use_external_mp(!same_object_and_instance);

    const c_float_or_percent max_detour = m_print_config.float_or_percent_or_default(
        "avoid_crossing_perimeters_max_detour", c_float_or_percent{0.0, 0});
    const Slic3r::AvoidCrossingPerimeters::TravelContext travel_context{
        *target_layer,
        local_source,
        Slic3r::Point{target_shift.x, target_shift.y},
        extruder_id,
        max_detour.value,
        max_detour.percent != 0
    };
    const Slic3r::Polyline routed = router.travel_to(travel_context, local_target);
    if (routed.size() < 2)
        return straight(source, target, extruder_id);

    std::vector<c_point> path;
    path.reserve(routed.size());
    for (const Slic3r::Point &point : routed.points)
        path.push_back(c_point{point.x() + target_shift.x, point.y() + target_shift.y});
    path.front() = c_point{source.position.x, source.position.y};
    path.back() = c_point{target.position.x, target.position.y};
    return path;
}

AvoidCrossingPerimetersTravel &AvoidCrossingPerimetersTravel::instance(orchestrator_handle *orchestrator)
{
    static AvoidCrossingPerimetersTravel plugin(orchestrator);
    return plugin;
}

AvoidCrossingPerimetersTravel::AvoidCrossingPerimetersTravel(orchestrator_handle *orchestrator) :
    PluginBase(orchestrator),
    m_entry_position_property(printing_layer_entry_position_property_key(orchestrator))
{}

const char *AvoidCrossingPerimetersTravel::id_impl() const noexcept
{
    return "layer_extrusion_edit.travel.avoid_crossing_perimeters";
}

const char *AvoidCrossingPerimetersTravel::name_impl() const noexcept
{
    return "Avoid crossing perimeters";
}

const char *AvoidCrossingPerimetersTravel::description_impl() const noexcept
{
    return "Reroutes eligible travels that would cross a perimeter.";
}

const char *AvoidCrossingPerimetersTravel::exclusive_group_impl() const noexcept
{
    return "layer_extrusion_edit.travel";
}

const char *AvoidCrossingPerimetersTravel::exclusive_group_label_impl() const noexcept
{
    return "Travel generation";
}

const char *AvoidCrossingPerimetersTravel::exclusive_group_tooltip_impl() const noexcept
{
    return "Selects how discontinuities between ordered extrusion leaves become travels.";
}

slicing_step_t AvoidCrossingPerimetersTravel::step_impl() const noexcept
{
    return STEP_LAYER_EXTRUSION_EDIT;
}

const char *const *AvoidCrossingPerimetersTravel::dependencies_impl() const noexcept
{
    return k_dependencies;
}

int32_t AvoidCrossingPerimetersTravel::priority_impl() const noexcept
{
    return -50;
}

int32_t AvoidCrossingPerimetersTravel::used_config_keys(raw_used_config_key *keys) const noexcept
{
    static const raw_used_config_key used_keys[] = {
        {"avoid_crossing_perimeters", RAW_CO_BOOL, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE},
        {"avoid_crossing_not_first_layer", RAW_CO_BOOL, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE},
        {"avoid_crossing_perimeters_max_detour", RAW_CO_FLOAT_OR_PERCENT, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE},
        {"nozzle_diameter", RAW_CO_VECTOR_FLOAT, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE},
        {"avoid_crossing_top", RAW_CO_BOOL, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE},
        {"avoid_travel_island", RAW_CO_BOOL, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE},
        {"avoid_travel_island_weight", RAW_CO_FLOAT, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE},
        {"external_perimeter_overlap", RAW_CO_PERCENT, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE},
        {"infill_extruder", RAW_CO_INT, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE},
        {"ironing", RAW_CO_BOOL, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE},
        {"perimeter_extruder", RAW_CO_INT, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE},
        {"perimeters", RAW_CO_INT, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE},
        {"solid_infill_extruder", RAW_CO_INT, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE}
    };
    if (keys != nullptr)
        std::copy(std::begin(used_keys), std::end(used_keys), keys);
    return int32_t(sizeof(used_keys) / sizeof(used_keys[0]));
}

const char *AvoidCrossingPerimetersTravel::progress_message_format_impl() const noexcept
{
    return "Connecting perimeter-avoiding travel paths: %u / %u trees";
}

void AvoidCrossingPerimetersTravel::setup_impl(const plugin_run_context *run_ctx, uint32_t) const
{
    const run_ctx_layer_extrusion_edition *ctx = plugin_ctx_as_layer_extrusion_edition(run_ctx);
    m_setup_valid = ctx != nullptr && ctx->print != nullptr && ctx->plan != nullptr;
}

void AvoidCrossingPerimetersTravel::setup_run_impl(const plugin_run_context *run_ctx) const
{
    if (!m_setup_valid)
        return;
    const run_ctx_layer_extrusion_edition *ctx = plugin_ctx_as_layer_extrusion_edition(run_ctx);
    if (ctx == nullptr || ctx->layer_group == nullptr)
        return;
    add_layer_progress(PrintingLayerGroup(ctx->layer_group), progress());
}

void AvoidCrossingPerimetersTravel::run_impl(const plugin_run_context *run_ctx) const
{
    if (!m_setup_valid)
        return;
    const run_ctx_layer_extrusion_edition *ctx = plugin_ctx_as_layer_extrusion_edition(run_ctx);
    if (ctx == nullptr || ctx->print == nullptr || ctx->layer_group == nullptr)
        return;

    const PrintingLayerGroup layer(ctx->layer_group);
    const PrintingLayerEntryPositionProperty *entry =
        m_entry_position_property.get(layer.properties());
    if (entry == nullptr)
        throw std::runtime_error(
            "Perimeter-avoiding travel generation requires the PrintingLayerGroup entry-position property.");
    if (entry->state != RAW_PRINTING_LAYER_ENTRY_POSITION_UNKNOWN &&
        entry->state != RAW_PRINTING_LAYER_ENTRY_POSITION_KNOWN)
        throw std::runtime_error("PrintingLayerGroup entry position has an invalid state.");

    PlannedPosition position = {};
    if (entry->is_known()) {
        position.x = entry->x;
        position.y = entry->y;
        position.z = entry->z;
        position.known = true;
    }

    LayerTravelPlanner planner(Print(ctx->print), run_ctx);
    const TravelPathPlanner route = [&planner](const TravelEndpoint &source,
                                               const TravelEndpoint &target,
                                               const uint16_t extruder_id) {
        return planner.path(source, target, extruder_id);
    };
    connect_layer(layer, position, route, progress());
}

} // namespace

void register_avoid_crossing_perimeters_travel_plugin(orchestrator_handle *orchestrator)
{
    orchestrator_register_plugin(
        orchestrator, AvoidCrossingPerimetersTravel::instance(orchestrator).c_instance());
}

}}} // namespace slic3r_api::LayerExtrusionEdit::AvoidCrossingPerimetersTravelPlugin
