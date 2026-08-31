///|/ Copyright (c) SuperSlicer 2026 Durand Remi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/

/*
Default layer extrusion acceleration
====================================

This plugin resolves acceleration on extrusion trees cloned into PrintingPlan.
Unlike speed, acceleration needs no group-wide flow analysis. setup_impl()
still validates all source attributes and configuration links before parallel
layer runs begin so an invalid later layer cannot leave earlier layers edited.

The plugin modifies only accel_mm_per_s2. Existing speed, pressure, fan, and
temperature fields remain owned by their original producers.
*/

#include "DefaultAcceleration.hpp"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <map>
#include <set>
#include <stdexcept>
#include <utility>
#include <vector>

#include "ExtrusionProcessParameterHelpers.hpp"
#include "RegionalProcessParameterHelpers.hpp"
#include "libslic3r/Api/plugin/c/steps/slic3r_step_layer_extrusion_edit.h"
#include "libslic3r/Api/plugin/cpp/PluginBase.hpp"

namespace slic3r_api { namespace LayerExtrusionEdit { namespace DefaultAccelerationPlugin {
namespace {

using namespace ProcessParameterHelpers;
using namespace RegionalProcessParameterHelpers;

const char *const k_no_dependencies[] = { nullptr };

const raw_used_config_key k_used_config_keys[] = {
    {"bridge_acceleration", RAW_CO_FLOAT_OR_PERCENT, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE},
    {"brim_acceleration", RAW_CO_FLOAT_OR_PERCENT, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE},
    {"default_acceleration", RAW_CO_FLOAT_OR_PERCENT, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE},
    {"external_perimeter_acceleration", RAW_CO_FLOAT_OR_PERCENT, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE},
    {"first_layer_acceleration", RAW_CO_FLOAT_OR_PERCENT, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE},
    {"gap_fill_acceleration", RAW_CO_FLOAT_OR_PERCENT, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE},
    {"infill_acceleration", RAW_CO_FLOAT_OR_PERCENT, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE},
    {"internal_bridge_acceleration", RAW_CO_FLOAT_OR_PERCENT, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE},
    {"ironing_acceleration", RAW_CO_FLOAT_OR_PERCENT, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE},
    {"machine_limits_usage", RAW_CO_ENUM, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE},
    {"machine_max_acceleration_extruding", RAW_CO_VECTOR_FLOAT, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE},
    {"machine_max_acceleration_travel", RAW_CO_VECTOR_FLOAT, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE},
    {"overhangs_acceleration", RAW_CO_FLOAT_OR_PERCENT, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE},
    {"perimeter_acceleration", RAW_CO_FLOAT_OR_PERCENT, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE},
    {"solid_infill_acceleration", RAW_CO_FLOAT_OR_PERCENT, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE},
    {"thin_walls_acceleration", RAW_CO_FLOAT_OR_PERCENT, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE},
    {"top_solid_infill_acceleration", RAW_CO_FLOAT_OR_PERCENT, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE},
    {"travel_acceleration", RAW_CO_FLOAT_OR_PERCENT, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE}
};

const RegionSettings::OptionKeyGroup k_region_acceleration_keys = {
    "default_acceleration",
    "bridge_acceleration",
    "external_perimeter_acceleration",
    "gap_fill_acceleration",
    "infill_acceleration",
    "internal_bridge_acceleration",
    "ironing_acceleration",
    "overhangs_acceleration",
    "perimeter_acceleration",
    "solid_infill_acceleration",
    "thin_walls_acceleration",
    "top_solid_infill_acceleration"
};

struct AccelerationPartitionKey
{
    RegionalSourceKey source;
    uint16_t extruder_id = uint16_t(-1);
    raw_extrusion_role role = RAW_EXTRUSION_ROLE_NONE;

    bool operator<(const AccelerationPartitionKey &rhs) const
    {
        if (source < rhs.source)
            return true;
        if (rhs.source < source)
            return false;
        if (extruder_id != rhs.extruder_id)
            return extruder_id < rhs.extruder_id;
        return uint16_t(role) < uint16_t(rhs.role);
    }
};

using AccelerationPartitions = std::map<AccelerationPartitionKey, RegionalProcessPartition>;
using RegionalSettingsPartitions = std::map<RegionalSourceKey, RegionalSettingsPartition>;

// Resolve role acceleration, then apply first-layer and machine limits.
double role_acceleration(const ExtrusionSettingsContext &context,
                         raw_extrusion_role role);

// Resolve travel acceleration without applying printable-role layer rules.
float resolved_travel_acceleration(const ExtrusionSettingsContext &context);

// Collect roles that still need acceleration from one validated tree.
void collect_unresolved_roles(MutableExtrusionEntity entity,
                              const EffectiveTreeState &parent_state,
                              std::set<raw_extrusion_role> &roles);

// Validate all trees and prepare immutable partitions before parallel mutation.
void prepare_plan(storage_handle *storage,
                  const Print &print,
                  const PrintingPlan &plan,
                  AccelerationPartitions &partitions);

// Assign missing acceleration recursively while preserving speed and overrides.
void assign_acceleration_tree(MutableExtrusionEntity entity,
                              const EffectiveTreeState &parent_state,
                              const ExtrusionSettingsContext &context,
                              const LayerRegionIsland &region_island,
                              uint16_t object_instance_idx,
                              const AccelerationPartitions &partitions,
                              ProcessFieldEditor &editor);

// Resolve and compact acceleration on one cloned extrusion root.
void edit_extrusion(const Print &print,
                    const PrintingToolGroup &tool_group,
                    const PrintingExtrusion &extrusion,
                    const AccelerationPartitions &partitions);

class DefaultAcceleration : public PluginBase
{
public:
    static DefaultAcceleration &instance(orchestrator_handle *orch);
    explicit DefaultAcceleration(orchestrator_handle *orch);

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

    mutable bool m_setup_valid = false;
    mutable AccelerationPartitions m_partitions;
};

double role_acceleration(const ExtrusionSettingsContext &context,
                         raw_extrusion_role role)
{
    // A missing default means acceleration remains unresolved. Every
    // role-specific percentage below is rooted in this default.
    const Config &region = context.region_config;
    double acceleration = region.computed_float_or_default(
        "default_acceleration", int32_t(context.extruder_id), 0.0);
    if (acceleration <= 0.0)
        return 0.0;

    const double perimeter = effective_value(region, "perimeter_acceleration", acceleration);
    const double external = effective_value(region, "external_perimeter_acceleration", perimeter);
    const double solid = effective_value(region, "solid_infill_acceleration", acceleration);
    const double bridge = effective_value(region, "bridge_acceleration", acceleration);

    // Select the most specific role override while retaining the family
    // fallback for disabled or zero options.
    if (RAW_EXTRUSION_ROLE_IS_SKIRT(role))
        acceleration = effective_value(context.object_config, "brim_acceleration", acceleration);
    else if (role == RAW_EXTRUSION_ROLE_GAP_FILL)
        acceleration = effective_value(region, "gap_fill_acceleration", perimeter);
    else if (role == RAW_EXTRUSION_ROLE_THIN_WALL)
        acceleration = effective_value(region, "thin_walls_acceleration", external);
    else if (RAW_EXTRUSION_ROLE_HAS(role, RAW_EXTRUSION_ROLE_IRONING))
        acceleration = effective_value(region, "ironing_acceleration",
            effective_value(region, "top_solid_infill_acceleration", solid));
    else if (RAW_EXTRUSION_ROLE_IS_INFILL(role)) {
        if (RAW_EXTRUSION_ROLE_IS_BRIDGE(role))
            acceleration = RAW_EXTRUSION_ROLE_IS_EXTERNAL(role) ? bridge :
                effective_value(region, "internal_bridge_acceleration", bridge);
        else if (RAW_EXTRUSION_ROLE_HAS(role, RAW_EXTRUSION_ROLE_SOLID))
            acceleration = RAW_EXTRUSION_ROLE_IS_EXTERNAL(role) ?
                effective_value(region, "top_solid_infill_acceleration", solid) : solid;
        else
            acceleration = effective_value(region, "infill_acceleration", solid);
    } else if (RAW_EXTRUSION_ROLE_IS_PERIMETER(role)) {
        acceleration = RAW_EXTRUSION_ROLE_IS_BRIDGE(role) ?
            effective_value(region, "overhangs_acceleration", bridge) :
            (RAW_EXTRUSION_ROLE_IS_EXTERNAL(role) ? external : perimeter);
    }

    // First-layer acceleration and the active machine limit are both ceilings;
    // neither rule may raise a slower role-specific value.
    if (context.first_layer) {
        const double first_layer = effective_value(
            context.object_config, "first_layer_acceleration", acceleration);
        if (first_layer > 0.0)
            acceleration = std::min(acceleration, first_layer);
    }

    const int32_t limits_usage = context.print_config.enum_or_default("machine_limits_usage", 3);
    if (limits_usage <= 2) {
        const double machine_limit = context.print_config.vector_float_or_default(
            "machine_max_acceleration_extruding", 0, 0.0);
        if (machine_limit > 0.0)
            acceleration = std::min(acceleration, machine_limit);
    }
    return acceleration;
}

float resolved_travel_acceleration(const ExtrusionSettingsContext &context)
{
    // computed_float_or_default() resolves a percentage against
    // default_acceleration. A zero travel value follows the documented legacy
    // fallback and uses that same default directly.
    double acceleration = context.region_config.computed_float_or_default(
        "travel_acceleration", int32_t(context.extruder_id), 0.0);
    if (acceleration <= 0.0)
        acceleration = context.region_config.computed_float_or_default(
            "default_acceleration", int32_t(context.extruder_id), 0.0);

    // Machine travel acceleration is an independent physical ceiling. It does
    // not reuse the extrusion ceiling merely because both live in one payload.
    const int32_t limits_usage = context.print_config.enum_or_default("machine_limits_usage", 3);
    if (limits_usage <= 2) {
        const double machine_limit = context.print_config.vector_float_or_default(
            "machine_max_acceleration_travel", 0, 0.0);
        if (machine_limit > 0.0)
            acceleration = std::min(acceleration, machine_limit);
    }

    if (acceleration <= 0.0 || !std::isfinite(acceleration))
        throw std::runtime_error("Unable to resolve a positive finite travel acceleration.");
    const float stored_acceleration = float(acceleration);
    if (!std::isfinite(stored_acceleration))
        throw std::runtime_error("Travel acceleration cannot be represented by the stored float.");
    return stored_acceleration;
}

void collect_unresolved_roles(MutableExtrusionEntity entity,
                              const EffectiveTreeState &parent_state,
                              std::set<raw_extrusion_role> &roles)
{
    const EffectiveTreeState state = effective_state(entity, parent_state);
    if (entity.child_count() > 0) {
        for (uint32_t child_idx = 0; child_idx < entity.child_count(); ++child_idx)
            collect_unresolved_roles(entity.child_mutable(child_idx), state, roles);
        return;
    }

    if (leaf_disposition(entity, state) == LeafDisposition::Editable && state.acceleration <= 0.f)
        roles.insert(state.attributes.extrusion_role());
}

void prepare_plan(storage_handle *storage,
                  const Print &print,
                  const PrintingPlan &plan,
                  AccelerationPartitions &partitions)
{
    partitions.clear();
    std::set<AccelerationPartitionKey> prepared_keys;
    RegionalSettingsPartitions regional_settings;

    // Visit every root before mutation. Besides validating source attributes,
    // collect every role that may need a regional partition in later runs.
    for (uint32_t group_idx = 0; group_idx < plan.group_count(); ++group_idx) {
        const PrintingGroup group = plan.group(group_idx);
        for (uint32_t layer_idx = 0; layer_idx < group.layer_group_count(); ++layer_idx) {
            const PrintingLayerGroup layer_group = group.layer_group(layer_idx);
            for (uint32_t tool_idx = 0; tool_idx < layer_group.tool_group_count(); ++tool_idx) {
                const PrintingToolGroup tool_group = layer_group.tool_group(tool_idx);
                for (uint32_t extrusion_idx = 0; extrusion_idx < tool_group.extrusion_count(); ++extrusion_idx) {
                    const PrintingExtrusion extrusion = tool_group.extrusion(extrusion_idx);
                    const ExtrusionSettingsContext context = validated_settings_context(
                        print, tool_group, extrusion, false);
                    const LayerRegionIsland region_island = extrusion.region_island();

                    std::set<raw_extrusion_role> roles;
                    collect_unresolved_roles(extrusion.mutable_root(), EffectiveTreeState{}, roles);
                    for (const raw_extrusion_role role : roles) {
                        const RegionalSourceKey source{
                            region_island.handle(), extrusion.object_instance_idx()
                        };
                        const AccelerationPartitionKey key{
                            source, tool_group.extruder_id(), role
                        };
                        if (!prepared_keys.insert(key).second)
                            continue;

                        // The geometry and raw setting tuples depend only on
                        // the source region-island and object instance. Reuse
                        // that expensive partition for every role and tool.
                        RegionalSettingsPartitions::iterator settings_it = regional_settings.find(source);
                        if (settings_it == regional_settings.end()) {
                            RegionalSettingsPartition settings = build_regional_settings_partition(
                                storage, region_island, extrusion.object_instance_idx(),
                                k_region_acceleration_keys);
                            settings_it = regional_settings.emplace(source, std::move(settings)).first;
                        }

                        RegionalProcessPartition partition = project_regional_process_values(
                            storage, settings_it->second,
                            [&context, role](const Config &region_config) {
                                ExtrusionSettingsContext regional_context = context;
                                regional_context.region_config = region_config;
                                return float(role_acceleration(regional_context, role));
                            });
                        if (partition.requires_split())
                            partitions.emplace(key, std::move(partition));
                    }
                }
            }
        }
    }
}

void assign_acceleration_tree(MutableExtrusionEntity entity,
                              const EffectiveTreeState &parent_state,
                              const ExtrusionSettingsContext &context,
                              const LayerRegionIsland &region_island,
                              uint16_t object_instance_idx,
                              const AccelerationPartitions &partitions,
                              ProcessFieldEditor &editor)
{
    // Collections propagate inherited state; only editable leaves receive a
    // missing acceleration value.
    const EffectiveTreeState state = effective_state(entity, parent_state);
    if (entity.child_count() > 0) {
        for (uint32_t child_idx = 0; child_idx < entity.child_count(); ++child_idx)
            assign_acceleration_tree(entity.child_mutable(child_idx), state, context,
                                     region_island, object_instance_idx, partitions, editor);
        return;
    }

    const LeafDisposition disposition = leaf_disposition(entity, state);
    if (disposition == LeafDisposition::Empty || state.acceleration > 0.f)
        return;

    const raw_extrusion_role role = state.attributes.extrusion_role();
    // Travels bypass regional partitioning and first-layer role policy. Their
    // single movement setting is resolved directly from the process config.
    if (RAW_EXTRUSION_ROLE_IS_TRAVEL(role)) {
        editor.set_value(entity, resolved_travel_acceleration(context));
        return;
    }
    if (disposition != LeafDisposition::Editable)
        return;

    const RegionalSourceKey source{ region_island.handle(), object_instance_idx };
    const AccelerationPartitionKey key{
        source, context.extruder_id, role
    };
    const AccelerationPartitions::const_iterator partition_it = partitions.find(key);
    if (partition_it == partitions.end()) {
        editor.set_value(entity, float(role_acceleration(context, role)));
        return;
    }

    // The common helper keeps clipping, fragment ordering and property edits
    // identical between acceleration and speed.
    apply_regional_process_partition(entity, partition_it->second, editor);
}

void edit_extrusion(const Print &print,
                    const PrintingToolGroup &tool_group,
                    const PrintingExtrusion &extrusion,
                    const AccelerationPartitions &partitions)
{
    const ExtrusionSettingsContext context = settings_context(print, tool_group, extrusion);
    const LayerRegionIsland region_island = extrusion.region_island();
    MutableExtrusionEntity root = extrusion.mutable_root();
    ProcessFieldEditor editor(ProcessField::Acceleration);
    assign_acceleration_tree(root, EffectiveTreeState{}, context, region_island,
                             extrusion.object_instance_idx(), partitions, editor);
    editor.hoist(root);
}

DefaultAcceleration &DefaultAcceleration::instance(orchestrator_handle *orch)
{
    static DefaultAcceleration singleton(orch);
    return singleton;
}

DefaultAcceleration::DefaultAcceleration(orchestrator_handle *orch) : PluginBase(orch) {}

const char *DefaultAcceleration::id_impl() const noexcept
{
    return "layer_extrusion_edit.acceleration.default";
}

const char *DefaultAcceleration::name_impl() const noexcept
{
    return "Default extrusion acceleration";
}

const char *DefaultAcceleration::description_impl() const noexcept
{
    return "Resolves missing extrusion acceleration and applies machine limits.";
}

const char *DefaultAcceleration::exclusive_group_impl() const noexcept
{
    return "layer_extrusion_edit.acceleration";
}

const char *DefaultAcceleration::exclusive_group_label_impl() const noexcept
{
    return "Extrusion acceleration";
}

const char *DefaultAcceleration::exclusive_group_tooltip_impl() const noexcept
{
    return "Selects how ordered layer extrusions receive their process acceleration.";
}

slicing_step_t DefaultAcceleration::step_impl() const noexcept
{
    return STEP_LAYER_EXTRUSION_EDIT;
}

const char *const *DefaultAcceleration::dependencies_impl() const noexcept
{
    return k_no_dependencies;
}

int32_t DefaultAcceleration::priority_impl() const noexcept
{
    return 10;
}

int32_t DefaultAcceleration::used_config_keys(raw_used_config_key *keys) const noexcept
{
    if (keys != nullptr)
        std::copy(std::begin(k_used_config_keys), std::end(k_used_config_keys), keys);
    return int32_t(std::size(k_used_config_keys));
}

const char *DefaultAcceleration::progress_message_format_impl() const noexcept
{
    return "Resolving extrusion acceleration: %u / %u trees";
}

void DefaultAcceleration::setup_impl(const plugin_run_context *run_ctx, uint32_t) const
{
    m_setup_valid = false;
    m_partitions.clear();
    const run_ctx_layer_extrusion_edition *ctx = plugin_ctx_as_layer_extrusion_edition(run_ctx);
    assert(ctx != nullptr && ctx->print != nullptr && ctx->plan != nullptr &&
           run_ctx != nullptr && run_ctx->plugin_storage != nullptr);
    if (ctx == nullptr || ctx->print == nullptr || ctx->plan == nullptr ||
        run_ctx == nullptr || run_ctx->plugin_storage == nullptr)
        return;

    prepare_plan(run_ctx->plugin_storage, Print(ctx->print), PrintingPlan(ctx->plan), m_partitions);
    m_setup_valid = true;
}

void DefaultAcceleration::setup_run_impl(const plugin_run_context *run_ctx) const
{
    const run_ctx_layer_extrusion_edition *ctx = plugin_ctx_as_layer_extrusion_edition(run_ctx);
    if (ctx == nullptr || ctx->layer_group == nullptr)
        return;
    progress().add_max(extrusion_tree_count(PrintingLayerGroup(ctx->layer_group)));
}

void DefaultAcceleration::run_impl(const plugin_run_context *run_ctx) const
{
    if (!m_setup_valid)
        return;
    const run_ctx_layer_extrusion_edition *ctx = plugin_ctx_as_layer_extrusion_edition(run_ctx);
    assert(ctx != nullptr && ctx->print != nullptr && ctx->layer_group != nullptr);
    if (ctx == nullptr || ctx->print == nullptr || ctx->layer_group == nullptr)
        return;

    const Print print(ctx->print);
    const PrintingLayerGroup layer_group(ctx->layer_group);
    for (uint32_t tool_idx = 0; tool_idx < layer_group.tool_group_count(); ++tool_idx) {
        const PrintingToolGroup tool_group = layer_group.tool_group(tool_idx);
        for (uint32_t extrusion_idx = 0; extrusion_idx < tool_group.extrusion_count(); ++extrusion_idx) {
            edit_extrusion(print, tool_group, tool_group.extrusion(extrusion_idx), m_partitions);
            progress().increment();
        }
    }
}

} // namespace

void register_default_acceleration_plugin(orchestrator_handle *orch)
{
    orchestrator_register_plugin(orch, DefaultAcceleration::instance(orch).c_instance());
}

}}} // namespace slic3r_api::LayerExtrusionEdit::DefaultAccelerationPlugin
