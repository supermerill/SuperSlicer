///|/ Copyright (c) SuperSlicer 2026 Durand Rémi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/

#include "SupportDemandBridgeRemoval.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iterator>
#include <utility>
#include <vector>

#include "libslic3r/Api/plugin/c/slic3r_config_def.h"
#include "libslic3r/Api/plugin/c/slic3r_orchestrator.h"
#include "libslic3r/Api/plugin/cpp/Views.hpp"

namespace slic3r_api { namespace Support { namespace SupportDemandBridgeRemovalPlugin {

namespace {

const char *k_support_demand_bridge_removal_id = "support.demand.bridge_removal";
const char *k_dependencies[] = { nullptr };
const raw_used_config_key k_used_config_keys[] = {
    { "dont_support_bridges", RAW_CO_BOOL, RAW_CONTAINER_TYPE_OBJECT, RAW_PRESET_TYPE_FFF_PRINT },
    { "support_material", RAW_CO_BOOL, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE },
    { "raft_layers", RAW_CO_INT, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE }
};

struct BridgeRemovalConfig
{
    bool dont_support_bridges = false;
};

struct BridgeLineFlow
{
    coord_t endpoint_extension = scale_i(0.4);
    double half_width = scale_d(0.2);
    double support_probe_offset = scale_d(0.2);
};

BridgeRemovalConfig read_object_config(const Object &object)
{
    const Config object_config = object.config();
    BridgeRemovalConfig out;
    out.dont_support_bridges = object_config.get("dont_support_bridges").get_bool();
    return out;
}

uint32_t island_work_count(const Object &object)
{
    uint32_t count = 0;
    for (uint32_t layer_idx = 1; layer_idx < object.layer_count(); ++layer_idx)
        count += object.layer(layer_idx).island_count();
    return count;
}

BridgeLineFlow bridge_line_flow(const LayerIsland &island)
{
    BridgeLineFlow out;
    bool found = false;
    for (uint32_t region_idx = 0; region_idx < island.region_count(); ++region_idx) {
        const c_flow flow = island.region(region_idx).flow(RAW_EXTRUSION_ROLE_EXTERNAL_PERIMETER);
        if (flow.width <= 0)
            continue;

        out.endpoint_extension = found ? std::min(out.endpoint_extension, flow.width) : flow.width;
        const coord_t effective_spacing = flow.spacing > 0 ? flow.spacing : flow.width;
        const double half_width = 0.5 * double(std::max(flow.width, effective_spacing)) + scale_d(0.001);
        out.half_width = found ? std::max(out.half_width, half_width) : half_width;
        const double support_probe_offset = 0.5 * double(flow.nozzle_diameter > 0 ? flow.nozzle_diameter : flow.width);
        out.support_probe_offset = found ? std::max(out.support_probe_offset, support_probe_offset) : support_probe_offset;
        found = true;
    }
    return out;
}

bool point_supported_by_expolygons(const ExPolygonCollection &support_polygons, c_point point)
{
    for (ExPolygon slice : support_polygons)
        if (slice.contains(point))
            return true;
    return false;
}

double segment_parameter(c_point point, c_point segment_a, c_point segment_b)
{
    const coord_t dx = segment_b.x - segment_a.x;
    const coord_t dy = segment_b.y - segment_a.y;
    if (std::abs(dx) >= std::abs(dy))
        return dx == 0 ? 0. : double(point.x - segment_a.x) / double(dx);
    return dy == 0 ? 0. : double(point.y - segment_a.y) / double(dy);
}

c_point segment_point_at(c_point segment_a, c_point segment_b, double parameter)
{
    c_point out = {};
    out.x = coord_t(std::llround(double(segment_a.x) + double(segment_b.x - segment_a.x) * parameter));
    out.y = coord_t(std::llround(double(segment_a.y) + double(segment_b.y - segment_a.y) * parameter));
    return out;
}

void add_polygon_line_intersections(const Polygon &polygon,
                                    c_point segment_a,
                                    c_point segment_b,
                                    std::vector<double> &parameters)
{
    const multipoint_const_view polygon_view = polygon.view();
    const uint32_t intersection_count = points_intersections(&polygon_view, segment_a, segment_b, nullptr);
    if (intersection_count == 0)
        return;

    std::vector<c_point> intersections(intersection_count);
    multipoint_view out_intersections = {};
    out_intersections.array = intersections.data();
    out_intersections.size = intersection_count;
    points_intersections(&polygon_view, segment_a, segment_b, &out_intersections);

    for (c_point point : intersections) {
        const double parameter = segment_parameter(point, segment_a, segment_b);
        if (parameter >= -1e-9 && parameter <= 1. + 1e-9)
            parameters.push_back(std::clamp(parameter, 0., 1.));
    }
}

std::vector<double> lower_support_intersection_parameters(const ExPolygonCollection &lower_support,
                                                          c_point segment_a,
                                                          c_point segment_b)
{
    std::vector<double> parameters;
    parameters.reserve(8);
    parameters.push_back(0.);
    parameters.push_back(1.);

    for (ExPolygon support_area : lower_support) {
        add_polygon_line_intersections(support_area.contour(), segment_a, segment_b, parameters);
        for (Polygon hole : support_area.holes())
            add_polygon_line_intersections(hole, segment_a, segment_b, parameters);
    }

    std::sort(parameters.begin(), parameters.end());
    parameters.erase(std::unique(parameters.begin(), parameters.end(),
                                 [](double lhs, double rhs) { return std::abs(lhs - rhs) < 1e-9; }),
                     parameters.end());
    return parameters;
}

void set_demand_from_operand(const run_ctx_support_demand &ctx,
                             const LayerIsland &island,
                             ClipperOperand &&operand)
{
    StoredExPolygonCollection polygons = operand.to_expolygon_collection();
    polygons.ensure_valid();
    ctx.set(ctx.demand, island.handle(), polygons.mutable_handle());
}

void append_bridge_segment_area(storage_handle *storage,
                                const ExPolygonCollection &lower_support,
                                const BridgeLineFlow &flow,
                                const ClipperContext &clipper,
                                const c_extrusion_segment &segment,
                                ClipperOperand &bridges)
{
    if (segment.radius != 0.f || c_point_distance_to(segment.point_a, segment.point_b) <= SCALED_EPSILON)
        return;

    // The old support code first subtracts the lower layer from perimeter
    // polylines, then checks whether each remaining straight unsupported piece
    // is anchored at both ends. Rebuild that split locally: every interval
    // between lower-support intersections is sampled, and only unsupported
    // intervals whose extended endpoints land on support are considered real
    // bridges.
    const std::vector<double> parameters =
        lower_support_intersection_parameters(lower_support, segment.point_a, segment.point_b);
    for (size_t idx = 1; idx < parameters.size(); ++idx) {
        const double begin = parameters[idx - 1];
        const double end = parameters[idx];
        if (end - begin <= 1e-9)
            continue;

        const c_point begin_point = segment_point_at(segment.point_a, segment.point_b, begin);
        const c_point end_point = segment_point_at(segment.point_a, segment.point_b, end);
        if (c_point_distance_to(begin_point, end_point) <= SCALED_EPSILON)
            continue;

        const c_point middle = segment_point_at(segment.point_a, segment.point_b, 0.5 * (begin + end));
        const bool middle_supported = point_supported_by_expolygons(lower_support, middle);
        if (middle_supported)
            continue;

        StoredPolyline line(storage);
        line.push_back(begin_point);
        line.push_back(end_point);
        line.extend_start(flow.endpoint_extension);
        line.extend_end(flow.endpoint_extension);
        const bool front_supported = point_supported_by_expolygons(lower_support, line.front());
        const bool back_supported = point_supported_by_expolygons(lower_support, line.back());
        if (!front_supported || !back_supported)
            continue;

        ClipperOperand bridge_area = clipper_offset(clipper(line), flow.half_width, CLIPPER_JOIN_SQUARE, 0.,
                                                    CLIPPER_END_OPEN_SQUARE);
        if (!bridge_area.empty())
            bridges.concat_replace(bridge_area);
    }
}

void append_bridge_areas_from_entity(storage_handle *storage,
                                     const ExPolygonCollection &lower_support,
                                     const BridgeLineFlow &flow,
                                     const ClipperContext &clipper,
                                     const ExtrusionEntity &entity,
                                     ClipperOperand &bridges)
{
    // The extrusion tree may contain collection nodes. Only local polylines can
    // describe printable bridge spans, so recurse until we find leaves carrying
    // segments.
    for (uint32_t segment_idx = 0; segment_idx < entity.segment_count(); ++segment_idx)
        append_bridge_segment_area(storage, lower_support, flow, clipper, entity.segment(segment_idx), bridges);

    for (uint32_t child_idx = 0; child_idx < entity.child_count(); ++child_idx)
        append_bridge_areas_from_entity(storage, lower_support, flow, clipper, entity.child(child_idx), bridges);
}

void append_bridge_areas_from_region_island(storage_handle *storage,
                                            const ExPolygonCollection &lower_support,
                                            const BridgeLineFlow &flow,
                                            const ClipperContext &clipper,
                                            const LayerRegionIsland &region_island,
                                            raw_extrusion_role role,
                                            ClipperOperand &bridges)
{
    if (!region_island.has_extrusion(role))
        return;

    const ExtrusionEntity root(region_island.extrusion(role));
    append_bridge_areas_from_entity(storage, lower_support, flow, clipper,
                                    root, bridges);
}

ClipperOperand bridge_areas_for_island(storage_handle *storage,
                                       const Layer &lower_layer,
                                       const LayerIsland &island,
                                       const ClipperContext &clipper)
{
    ClipperOperand bridges = clipper.empty();
    const BridgeLineFlow flow = bridge_line_flow(island);
    ClipperOperand lower_support_operand = clipper_offset(clipper(lower_layer.slices()),
                                                          flow.support_probe_offset,
                                                          CLIPPER_JOIN_SQUARE,
                                                          0.);
    StoredExPolygonCollection lower_support = lower_support_operand.to_expolygon_collection();
    lower_support.ensure_valid();

    for (uint32_t region_island_idx = 0; region_island_idx < island.region_island_count(); ++region_island_idx) {
        const LayerRegionIsland region_island = island.region_island(region_island_idx);
        append_bridge_areas_from_region_island(storage, lower_support, flow, clipper, region_island,
                                               RAW_EXTRUSION_ROLE_PERIMETER, bridges);
        append_bridge_areas_from_region_island(storage, lower_support, flow, clipper, region_island,
                                               RAW_EXTRUSION_ROLE_GAP_FILL, bridges);
    }

    return bridges.empty() ? std::move(bridges) : clipper_union(bridges);
}

void remove_bridges_from_island_demand(const run_ctx_support_demand &ctx,
                                       storage_handle *storage,
                                       const Layer &lower_layer,
                                       const LayerIsland &island,
                                       const ClipperContext &clipper)
{
    expolygon_collection_handle *existing_handle = ctx.get(ctx.demand, island.handle());
    if (existing_handle == nullptr)
        return;

    ClipperOperand bridges = bridge_areas_for_island(storage, lower_layer, island, clipper);
    if (bridges.empty())
        return;

    ExPolygonCollection existing(existing_handle);
    ClipperOperand remaining = clipper_diff_with_safety_offset(clipper(existing), bridges);
    set_demand_from_operand(ctx, island, std::move(remaining));
}

} // namespace

SupportDemandBridgeRemoval &
SupportDemandBridgeRemoval::instance(orchestrator_handle *orch)
{
    static SupportDemandBridgeRemoval s_instance(orch);
    return s_instance;
}

const char *SupportDemandBridgeRemoval::id_impl() const noexcept
{
    return k_support_demand_bridge_removal_id;
}

const char *SupportDemandBridgeRemoval::name_impl() const noexcept
{
    return "Support demand bridge removal";
}

const char *SupportDemandBridgeRemoval::description_impl() const noexcept
{
    return "Remove support demand under bridge spans when bridge support is disabled.";
}

slicing_step_t SupportDemandBridgeRemoval::step_impl() const noexcept
{
    return STEP_SUPPORT_DEMAND;
}

const char *const *SupportDemandBridgeRemoval::dependencies_impl() const noexcept
{
    return k_dependencies;
}

int32_t SupportDemandBridgeRemoval::priority_impl() const noexcept
{
    return 15;
}

int32_t SupportDemandBridgeRemoval::used_config_keys(raw_used_config_key *keys) const noexcept
{
    if (keys != nullptr)
        std::copy(std::begin(k_used_config_keys), std::end(k_used_config_keys), keys);
    return int32_t(std::size(k_used_config_keys));
}

int32_t SupportDemandBridgeRemoval::defined_config_keys(const char **keys) const noexcept
{
    if (keys != nullptr)
        keys[0] = k_used_config_keys[0].key;
    return 1;
}

const char *SupportDemandBridgeRemoval::progress_message_format_impl() const noexcept
{
    return "Support demand bridge removal: %u / %u islands";
}

const char *SupportDemandBridgeRemoval::print_ui_fragment() noexcept
{
    return "page:Support material\n"
           "group:Options for support material and raft\n"
           "setting:insert$aftersetting$support_material_xy_spacing:dont_support_bridges\n";
}

void SupportDemandBridgeRemoval::inilialize_impl(storage_handle *) const
{
    raw_config_option_def def = raw_config_option_def_init();
    def.opt_key = "dont_support_bridges";
    def.type = RAW_CO_BOOL;
    def.container_type = RAW_CONTAINER_TYPE_OBJECT;
    def.option_preset_type = RAW_PRESET_TYPE_FFF_PRINT;
    def.printer_technology = RAW_PT_FFF;
    def.label = "Don't support bridges";
    def.category = RAW_OPTION_CATEGORY_SUPPORT;
    def.invalidates_step = STEP_SLICING;
    def.tooltip = "Experimental option for preventing support material from being generated under bridged areas.";
    def.mode = RAW_CONFIG_OPTION_MODE_ADV_EXP | RAW_CONFIG_OPTION_MODE_PRUSA;
    def.default_serialized_value = "1";
    orchestrator_create_option_def(m_orchestrator, &def);

    orchestrator_add_ui_fragment(m_orchestrator,
                                 "print.ui",
                                 k_support_demand_bridge_removal_id,
                                 SupportDemandBridgeRemoval::print_ui_fragment(),
                                 0);

    raw_gui_rule rule = raw_gui_rule_init();
    rule.action = RAW_GUI_RULE_ACTION_ENABLE_ANY;
    rule.condition = RAW_GUI_RULE_CONDITION_BOOL_TRUE;
    rule.condition_key = "support_material";
    rule.target_key = "dont_support_bridges";
    orchestrator_add_gui_rule(m_orchestrator, &rule);

    rule = raw_gui_rule_init();
    rule.action = RAW_GUI_RULE_ACTION_ENABLE_ANY;
    rule.condition = RAW_GUI_RULE_CONDITION_VALUE_NON_ZERO;
    rule.condition_key = "raft_layers";
    rule.target_key = "dont_support_bridges";
    orchestrator_add_gui_rule(m_orchestrator, &rule);
}

void SupportDemandBridgeRemoval::setup_run_impl(const plugin_run_context *run_ctx) const
{
    const run_ctx_support_demand *ctx = plugin_ctx_as_support_demand(run_ctx);
    if (ctx == nullptr || ctx->object == nullptr)
        return;

    const Object object(ctx->object);
    const BridgeRemovalConfig config = read_object_config(object);
    if (config.dont_support_bridges)
        progress().add_max(island_work_count(object));
}

void SupportDemandBridgeRemoval::run_impl(const plugin_run_context *run_ctx) const
{
    const run_ctx_support_demand *ctx = plugin_ctx_as_support_demand(run_ctx);
    if (ctx == nullptr || ctx->object == nullptr || ctx->demand == nullptr)
        return;

    const Object object(ctx->object);
    const BridgeRemovalConfig config = read_object_config(object);
    if (!config.dont_support_bridges) {
        progress().finish_run();
        return;
    }

    storage_handle *storage = run_ctx->plugin_storage;
    ClipperContext clipper(storage);
    for (uint32_t layer_idx = 1; layer_idx < object.layer_count(); ++layer_idx) {
        throw_if_cancelled(run_ctx);

        const Layer layer = object.layer(layer_idx);
        const Layer lower_layer = object.layer(layer_idx - 1);
        for (uint32_t island_idx = 0; island_idx < layer.island_count(); ++island_idx) {
            throw_if_cancelled(run_ctx);

            const LayerIsland island = layer.island(island_idx);
            remove_bridges_from_island_demand(*ctx, storage, lower_layer, island, clipper);
            progress().increment();
        }
    }

    progress().finish_run();
}

void register_support_demand_bridge_removal_plugin(orchestrator_handle *orch)
{
    orchestrator_register_plugin(orch, SupportDemandBridgeRemoval::instance(orch).c_instance());
}

}}} // namespace slic3r_api::Support::SupportDemandBridgeRemovalPlugin

#ifdef SUPPORT_DEMAND_BRIDGE_REMOVAL_PLUGIN_DLL
extern "C" void register_plugin(orchestrator_handle *orch)
{
    slic3r_api::Support::SupportDemandBridgeRemovalPlugin::register_support_demand_bridge_removal_plugin(orch);
}
#endif // SUPPORT_DEMAND_BRIDGE_REMOVAL_PLUGIN_DLL
