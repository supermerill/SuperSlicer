///|/ Copyright (c) SuperSlicer 2026 Durand Remi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/

/*
Default layer extrusion speed
=============================

This plugin resolves speed on extrusion trees cloned into PrintingPlan. Its
setup pass validates every printable tree and computes immutable autospeed
targets per PrintingGroup and extruder. Parallel layer runs then resolve only
missing speed values, apply first-layer and physical limits, and compact
uniform speed towards parent nodes.

Acceleration is intentionally not handled here. A separate plugin may choose
and hoist acceleration without depending on this strategy.
*/

#include "DefaultSpeed.hpp"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <iterator>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <vector>

#include "ExtrusionProcessParameterHelpers.hpp"
#include "RegionalProcessParameterHelpers.hpp"
#include "libslic3r/Api/plugin/c/steps/slic3r_step_layer_extrusion_edit.h"
#include "libslic3r/Api/plugin/cpp/PluginBase.hpp"

namespace slic3r_api { namespace LayerExtrusionEdit { namespace DefaultSpeedPlugin {
namespace {

using namespace ProcessParameterHelpers;
using namespace RegionalProcessParameterHelpers;

const char *const k_no_dependencies[] = { nullptr };

const raw_used_config_key k_used_config_keys[] = {
    {"autospeed_min_thin_flow", RAW_CO_FLOAT_OR_PERCENT, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE},
    {"bridge_speed", RAW_CO_FLOAT_OR_PERCENT, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE},
    {"brim_speed", RAW_CO_FLOAT_OR_PERCENT, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE},
    {"external_perimeter_speed", RAW_CO_FLOAT_OR_PERCENT, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE},
    {"filament_max_speed", RAW_CO_VECTOR_FLOAT, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE},
    {"filament_max_volumetric_speed", RAW_CO_VECTOR_FLOAT, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE},
    {"first_layer_flow_ratio", RAW_CO_PERCENT, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE},
    {"first_layer_infill_speed", RAW_CO_FLOAT_OR_PERCENT, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE},
    {"first_layer_min_speed", RAW_CO_FLOAT, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE},
    {"first_layer_speed", RAW_CO_FLOAT_OR_PERCENT, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE},
    {"gap_fill_speed", RAW_CO_FLOAT_OR_PERCENT, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE},
    {"infill_speed", RAW_CO_FLOAT_OR_PERCENT, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE},
    {"internal_bridge_speed", RAW_CO_FLOAT_OR_PERCENT, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE},
    {"ironing_speed", RAW_CO_FLOAT_OR_PERCENT, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE},
    {"max_print_speed", RAW_CO_FLOAT_OR_PERCENT, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE},
    {"max_volumetric_speed", RAW_CO_FLOAT, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE},
    {"milling_speed", RAW_CO_FLOAT, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE},
    {"overhangs", RAW_CO_BOOL, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE},
    {"overhangs_speed", RAW_CO_FLOAT_OR_PERCENT, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE},
    {"perimeter_speed", RAW_CO_FLOAT_OR_PERCENT, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE},
    {"solid_infill_speed", RAW_CO_FLOAT_OR_PERCENT, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE},
    {"thin_walls_speed", RAW_CO_FLOAT_OR_PERCENT, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE},
    {"top_solid_infill_speed", RAW_CO_FLOAT_OR_PERCENT, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE},
    {"travel_speed", RAW_CO_FLOAT, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE}
};

const RegionSettings::OptionKeyGroup k_region_speed_keys = {
    "bridge_speed",
    "external_perimeter_speed",
    "gap_fill_speed",
    "infill_speed",
    "internal_bridge_speed",
    "ironing_speed",
    "milling_speed",
    "overhangs",
    "overhangs_speed",
    "perimeter_speed",
    "solid_infill_speed",
    "thin_walls_speed",
    "top_solid_infill_speed"
};

using AutospeedTargets = std::vector<std::map<uint16_t, double>>;
using RegionalSettingsPartitions = std::map<RegionalSourceKey, RegionalSettingsPartition>;

struct AutospeedPartitionKey
{
    RegionalSourceKey source;
    uint16_t extruder_id = uint16_t(-1);
    raw_extrusion_role role = RAW_EXTRUSION_ROLE_NONE;
    bool full_overhang_speed = false;

    bool operator<(const AutospeedPartitionKey &rhs) const;
};

struct SpeedPartitionKey
{
    RegionalSourceKey source;
    uint32_t group_idx = 0;
    uint16_t extruder_id = uint16_t(-1);
    raw_extrusion_role role = RAW_EXTRUSION_ROLE_NONE;
    bool full_overhang_speed = false;
    double mm3_per_mm = 0.0;
    bool has_volumetric_flow = false;

    bool operator<(const SpeedPartitionKey &rhs) const;
};

using AutospeedPartitions = std::map<AutospeedPartitionKey, RegionalProcessPartition>;
using SpeedPartitions = std::map<SpeedPartitionKey, RegionalProcessPartition>;

// Resolve the role speed through the legacy percentage fallback hierarchy.
double role_speed(const ExtrusionSettingsContext &context,
                  raw_extrusion_role role,
                  bool full_overhang_speed,
                  double auto_base);

// Apply first-layer rules and physical print/filament caps after role choice.
double finalize_speed(const ExtrusionSettingsContext &context,
                      raw_extrusion_role role,
                      double mm3_per_mm,
                      bool has_volumetric_flow,
                      double speed);

// Resolve the exact float later written to one leaf or regional fragment.
float resolved_speed(const ExtrusionSettingsContext &context,
                     raw_extrusion_role role,
                     bool full_overhang_speed,
                     double mm3_per_mm,
                     bool has_volumetric_flow,
                     double autospeed_target);

// Resolve travel speed independently from printable-role and volumetric rules.
float resolved_travel_speed(const ExtrusionSettingsContext &context);

// Scan unresolved eligible leaves for the group's smallest flow cross-section.
void collect_autospeed_minimum(const MutableExtrusionEntity &entity,
                              const EffectiveTreeState &parent_state,
                              const ExtrusionSettingsContext &context,
                              storage_handle *storage,
                              const RegionalSourceKey &source,
                              const RegionalSettingsPartition &regional_settings,
                              AutospeedPartitions &regional_partitions,
                              bool exclude_thin_flows,
                              double &minimum);

// Prepare raw/final regional partitions and group/extruder autospeed targets.
void prepare_plan(storage_handle *storage,
                  const Print &print,
                  const PrintingPlan &plan,
                  AutospeedTargets &targets,
                  SpeedPartitions &speed_partitions);

// Build final speed partitions required by unresolved leaves in one tree.
void collect_speed_partitions(MutableExtrusionEntity entity,
                              const EffectiveTreeState &parent_state,
                              const ExtrusionSettingsContext &context,
                              const RegionalSourceKey &source,
                              const RegionalSettingsPartition &regional_settings,
                              storage_handle *storage,
                              uint32_t group_idx,
                              double autospeed_target,
                              std::set<SpeedPartitionKey> &prepared_keys,
                              SpeedPartitions &speed_partitions);

// Assign missing speed recursively while preserving inherited overrides.
void assign_speed_tree(MutableExtrusionEntity entity,
                       const EffectiveTreeState &parent_state,
                       const ExtrusionSettingsContext &context,
                       uint32_t group_idx,
                       const RegionalSourceKey &source,
                       double autospeed_target,
                       const SpeedPartitions &speed_partitions,
                       ProcessFieldEditor &editor);

// Return a target for one group/extruder, or zero when autospeed is unavailable.
double autospeed_target(const AutospeedTargets &targets,
                        uint32_t group_idx,
                        uint16_t extruder_id);

// Resolve and compact speed on one cloned extrusion root.
void edit_extrusion(const Print &print,
                    const PrintingToolGroup &tool_group,
                    const PrintingExtrusion &extrusion,
                    uint32_t group_idx,
                    double target,
                    const SpeedPartitions &speed_partitions);

class DefaultSpeed : public PluginBase
{
public:
    static DefaultSpeed &instance(orchestrator_handle *orch);
    explicit DefaultSpeed(orchestrator_handle *orch);

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

    mutable AutospeedTargets m_autospeed_targets;
    mutable SpeedPartitions m_speed_partitions;
    mutable bool m_setup_valid = false;
};

bool AutospeedPartitionKey::operator<(const AutospeedPartitionKey &rhs) const
{
    if (source < rhs.source)
        return true;
    if (rhs.source < source)
        return false;
    if (extruder_id != rhs.extruder_id)
        return extruder_id < rhs.extruder_id;
    if (role != rhs.role)
        return uint16_t(role) < uint16_t(rhs.role);
    return full_overhang_speed < rhs.full_overhang_speed;
}

bool SpeedPartitionKey::operator<(const SpeedPartitionKey &rhs) const
{
    if (source < rhs.source)
        return true;
    if (rhs.source < source)
        return false;
    if (group_idx != rhs.group_idx)
        return group_idx < rhs.group_idx;
    if (extruder_id != rhs.extruder_id)
        return extruder_id < rhs.extruder_id;
    if (role != rhs.role)
        return uint16_t(role) < uint16_t(rhs.role);
    if (full_overhang_speed != rhs.full_overhang_speed)
        return full_overhang_speed < rhs.full_overhang_speed;
    if (mm3_per_mm != rhs.mm3_per_mm)
        return mm3_per_mm < rhs.mm3_per_mm;
    return has_volumetric_flow < rhs.has_volumetric_flow;
}

double role_speed(const ExtrusionSettingsContext &context,
                  raw_extrusion_role role,
                  bool full_overhang_speed,
                  double auto_base)
{
    // Build the shared fallback chain once. Role-specific percentages are
    // evaluated against the preceding value, ending at the autospeed base.
    const Config &region = context.region_config;
    const double perimeter = effective_value(region, "perimeter_speed", auto_base);
    const double external = effective_value(region, "external_perimeter_speed", perimeter);
    const double solid = effective_value(region, "solid_infill_speed", auto_base);
    const double bridge = effective_value(region, "bridge_speed", auto_base);

    // Resolve roles outside the perimeter and infill families first.
    if (RAW_EXTRUSION_ROLE_IS_SKIRT(role))
        return effective_value(context.object_config, "brim_speed", auto_base);
    if (RAW_EXTRUSION_ROLE_HAS(role, RAW_EXTRUSION_ROLE_MILL))
        return region.float_or_default("milling_speed", 0.0);
    if (role == RAW_EXTRUSION_ROLE_GAP_FILL)
        return effective_value(region, "gap_fill_speed", perimeter);
    if (role == RAW_EXTRUSION_ROLE_THIN_WALL)
        return effective_value(region, "thin_walls_speed", external);
    if (RAW_EXTRUSION_ROLE_HAS(role, RAW_EXTRUSION_ROLE_IRONING))
        return effective_value(region, "ironing_speed",
                               effective_value(region, "top_solid_infill_speed", solid));

    // Infill refines solid and bridge bases according to its exact role.
    if (RAW_EXTRUSION_ROLE_IS_INFILL(role)) {
        if (RAW_EXTRUSION_ROLE_IS_BRIDGE(role))
            return RAW_EXTRUSION_ROLE_IS_EXTERNAL(role) ? bridge :
                effective_value(region, "internal_bridge_speed", bridge);
        if (RAW_EXTRUSION_ROLE_HAS(role, RAW_EXTRUSION_ROLE_SOLID))
            return RAW_EXTRUSION_ROLE_IS_EXTERNAL(role) ?
                effective_value(region, "top_solid_infill_speed", solid) : solid;
        return effective_value(region, "infill_speed", solid);
    }

    // Fully unsupported perimeters use overhang speed; other perimeters keep
    // their internal or external fallback.
    if (RAW_EXTRUSION_ROLE_IS_PERIMETER(role)) {
        const bool use_overhang = full_overhang_speed ||
            (RAW_EXTRUSION_ROLE_IS_BRIDGE(role) && region.bool_or_default("overhangs", false));
        if (use_overhang)
            return effective_value(region, "overhangs_speed", bridge);
        return RAW_EXTRUSION_ROLE_IS_EXTERNAL(role) ? external : perimeter;
    }
    return 0.0;
}

double finalize_speed(const ExtrusionSettingsContext &context,
                      raw_extrusion_role role,
                      double mm3_per_mm,
                      bool has_volumetric_flow,
                      double speed)
{
    // Apply first-layer limits after role selection. Infill may use a distinct
    // cap, while all remaining roles share first_layer_speed.
    if (context.first_layer) {
        const double first_layer_speed = effective_value(context.object_config, "first_layer_speed", speed);
        if (role == RAW_EXTRUSION_ROLE_INTERNAL_INFILL || role == RAW_EXTRUSION_ROLE_SOLID_INFILL) {
            const double infill_speed = effective_value(
                context.object_config, "first_layer_infill_speed", speed);
            const double limit = infill_speed > 0.0 ? infill_speed : first_layer_speed;
            if (limit > 0.0)
                speed = std::min(speed, limit);
        } else if (first_layer_speed > 0.0) {
            speed = std::min(speed, first_layer_speed);
        }

        // The minimum follows the cap. The flow ratio affects only the
        // volumetric ceilings converted below.
        speed = std::max(speed, context.object_config.float_or_default("first_layer_min_speed", 0.0));
        if (has_volumetric_flow)
            mm3_per_mm *= effective_value(context.object_config, "first_layer_flow_ratio", 1.0);
    }

    // Apply each independent linear or volumetric ceiling without ever raising
    // the speed selected by the role hierarchy.
    const double max_print_speed = context.print_config.computed_float_or_default(
        "max_print_speed", int32_t(context.extruder_id), 0.0);
    if (max_print_speed > 0.0)
        speed = std::min(speed, max_print_speed);

    const double max_volumetric = context.print_config.float_or_default("max_volumetric_speed", 0.0);
    if (has_volumetric_flow && max_volumetric > 0.0 && mm3_per_mm > 0.0)
        speed = std::min(speed, max_volumetric / mm3_per_mm);

    const double filament_max_volumetric = context.print_config.vector_float_or_default(
        "filament_max_volumetric_speed", context.extruder_id, 0.0);
    if (has_volumetric_flow && filament_max_volumetric > 0.0 && mm3_per_mm > 0.0)
        speed = std::min(speed, filament_max_volumetric / mm3_per_mm);

    const double filament_max_speed = context.print_config.vector_float_or_default(
        "filament_max_speed", context.extruder_id, 0.0);
    if (filament_max_speed > 0.0)
        speed = std::min(speed, filament_max_speed);
    return speed;
}

float resolved_speed(const ExtrusionSettingsContext &context,
                     raw_extrusion_role role,
                     bool full_overhang_speed,
                     double mm3_per_mm,
                     bool has_volumetric_flow,
                     double autospeed_target_value)
{
    double speed = role_speed(context, role, full_overhang_speed, 0.0);

    // Convert a shared volumetric target into the linear base appropriate for
    // this exact flow, then reevaluate role percentages against that base.
    if (speed <= 0.0 && autospeed_target_value > 0.0 && has_volumetric_flow) {
        const double max_print_speed = context.print_config.computed_float_or_default(
            "max_print_speed", int32_t(context.extruder_id), 0.0);
        const double auto_base = std::min(autospeed_target_value / mm3_per_mm, max_print_speed);
        speed = role_speed(context, role, full_overhang_speed, auto_base);
        if (speed <= 0.0)
            speed = auto_base;
    }

    // With no volumetric target, max_print_speed remains the last legacy
    // fallback before first-layer and physical ceilings are applied.
    if (speed <= 0.0)
        speed = context.print_config.computed_float_or_default(
            "max_print_speed", int32_t(context.extruder_id), 0.0);
    if (speed <= 0.0 || !std::isfinite(speed))
        throw std::runtime_error("Unable to resolve a positive finite extrusion speed.");

    if (!RAW_EXTRUSION_ROLE_HAS(role, RAW_EXTRUSION_ROLE_MILL))
        speed = finalize_speed(context, role, mm3_per_mm, has_volumetric_flow, speed);
    if (speed <= 0.0 || !std::isfinite(speed))
        throw std::runtime_error("Extrusion speed limits produced an invalid value.");
    const float stored_speed = float(speed);
    if (!std::isfinite(stored_speed))
        throw std::runtime_error("Extrusion speed cannot be represented by the stored float.");
    return stored_speed;
}

float resolved_travel_speed(const ExtrusionSettingsContext &context)
{
    // Travel speed is a machine movement setting. It must not inherit role
    // percentages or be reduced by material-flow and first-layer ceilings.
    const double speed = context.print_config.float_or_default("travel_speed", 0.0);
    if (speed <= 0.0 || !std::isfinite(speed))
        throw std::runtime_error("Unable to resolve a positive finite travel speed.");
    const float stored_speed = float(speed);
    if (!std::isfinite(stored_speed))
        throw std::runtime_error("Travel speed cannot be represented by the stored float.");
    return stored_speed;
}

void collect_autospeed_minimum(const MutableExtrusionEntity &entity,
                              const EffectiveTreeState &parent_state,
                              const ExtrusionSettingsContext &context,
                              storage_handle *storage,
                              const RegionalSourceKey &source,
                              const RegionalSettingsPartition &regional_settings,
                              AutospeedPartitions &regional_partitions,
                              bool exclude_thin_flows,
                              double &minimum)
{
    // Walk to leaves with inherited properties resolved because intermediate
    // collection nodes do not carry a printable flow cross-section.
    const EffectiveTreeState state = effective_state(entity, parent_state);
    if (entity.child_count() > 0) {
        for (uint32_t child_idx = 0; child_idx < entity.child_count(); ++child_idx)
            collect_autospeed_minimum(entity.child_mutable(child_idx), state, context,
                                      storage, source, regional_settings, regional_partitions,
                                      exclude_thin_flows, minimum);
        return;
    }

    // Existing speed and roles with an explicit positive setting do not depend
    // on the shared target and therefore must not influence it.
    if (leaf_disposition(entity, state) != LeafDisposition::Editable || state.speed > 0.f)
        return;
    const raw_extrusion_role role = state.attributes.extrusion_role();
    if (state.attributes.c_extrusion_property_attributes::height == -2.f ||
        RAW_EXTRUSION_ROLE_HAS(role, RAW_EXTRUSION_ROLE_MILL) ||
        RAW_EXTRUSION_ROLE_HAS(role, RAW_EXTRUSION_ROLE_IRONING) ||
        (exclude_thin_flows && (role == RAW_EXTRUSION_ROLE_GAP_FILL || role == RAW_EXTRUSION_ROLE_THIN_WALL)))
        return;

    // Resolve auto-vs-explicit speed for every regional tuple, then split a
    // deep clone to inspect only the zones actually crossed by this leaf.
    const AutospeedPartitionKey partition_key{
        source, context.extruder_id, role, state.full_overhang_speed
    };
    AutospeedPartitions::iterator partition_it = regional_partitions.find(partition_key);
    if (partition_it == regional_partitions.end()) {
        RegionalProcessPartition partition = project_regional_process_values(
            storage, regional_settings,
            [&context, role, &state](const Config &region_config) {
                ExtrusionSettingsContext regional_context = context;
                regional_context.region_config = region_config;
                return float(role_speed(
                    regional_context, role, state.full_overhang_speed, 0.0));
            });
        partition_it = regional_partitions.emplace(partition_key, std::move(partition)).first;
    }
    if (!regional_partition_leaf_matches(
            storage, entity, partition_it->second,
            [](float regional_speed) { return regional_speed <= 0.f; }))
        return;

    // The thinnest eligible flow defines the linear speed required to reach a
    // common volumetric target.
    const double mm3_per_mm = state.attributes.c_extrusion_property_attributes::mm3_per_mm;
    if (mm3_per_mm > 0.0)
        minimum = std::min(minimum, mm3_per_mm);
}

void prepare_plan(storage_handle *storage,
                  const Print &print,
                  const PrintingPlan &plan,
                  AutospeedTargets &targets,
                  SpeedPartitions &speed_partitions)
{
    // Targets are isolated by printing group and extruder so complete-object
    // groups and different filaments cannot influence each other.
    targets.clear();
    targets.resize(plan.group_count());
    speed_partitions.clear();
    RegionalSettingsPartitions regional_settings;
    AutospeedPartitions autospeed_partitions;
    const Config print_config = print.config();
    const double maximum_volumetric = print_config.float_or_default("max_volumetric_speed", 0.0);
    const bool thin_floor_enabled = print_config.has("autospeed_min_thin_flow") &&
        print_config.get("autospeed_min_thin_flow").is_enabled();

    for (uint32_t group_idx = 0; group_idx < plan.group_count(); ++group_idx) {
        const PrintingGroup group = plan.group(group_idx);
        std::map<uint16_t, double> minimum_by_extruder;

        // Validate the entire plan before any parallel mutation. During the
        // same pass, collect the minimum flow when autospeed is enabled.
        for (uint32_t layer_idx = 0; layer_idx < group.layer_group_count(); ++layer_idx) {
            const PrintingLayerGroup layer_group = group.layer_group(layer_idx);
            for (uint32_t tool_idx = 0; tool_idx < layer_group.tool_group_count(); ++tool_idx) {
                const PrintingToolGroup tool_group = layer_group.tool_group(tool_idx);
                for (uint32_t extrusion_idx = 0; extrusion_idx < tool_group.extrusion_count(); ++extrusion_idx) {
                    const PrintingExtrusion extrusion = tool_group.extrusion(extrusion_idx);
                    const ExtrusionSettingsContext context = validated_settings_context(
                        print, tool_group, extrusion, true);
                    const LayerRegionIsland region_island = extrusion.region_island();
                    const RegionalSourceKey source{
                        region_island.handle(), extrusion.object_instance_idx()
                    };

                    // Build raw RegionSettings geometry once per source and
                    // translated object instance. Both autospeed analysis and
                    // final speed projection consume this immutable result.
                    RegionalSettingsPartitions::iterator settings_it = regional_settings.find(source);
                    if (settings_it == regional_settings.end()) {
                        RegionalSettingsPartition settings = build_regional_settings_partition(
                            storage, region_island, extrusion.object_instance_idx(),
                            k_region_speed_keys);
                        settings_it = regional_settings.emplace(source, std::move(settings)).first;
                    }
                    if (maximum_volumetric <= 0.0)
                        continue;

                    double &minimum = minimum_by_extruder[tool_group.extruder_id()];
                    if (minimum == 0.0)
                        minimum = (std::numeric_limits<double>::max)();
                    collect_autospeed_minimum(extrusion.mutable_root(), EffectiveTreeState{}, context,
                                              storage, source, settings_it->second,
                                              autospeed_partitions, thin_floor_enabled, minimum);
                }
            }
        }

        // Without a volumetric ceiling, assignment uses max_print_speed as its
        // final fallback and no autospeed target is materialized.
        if (maximum_volumetric <= 0.0)
            continue;

        for (std::map<uint16_t, double>::value_type &entry : minimum_by_extruder) {
            const uint16_t extruder_id = entry.first;
            const double max_print_speed = print_config.computed_float_or_default(
                "max_print_speed", int32_t(extruder_id), 0.0);
            if (max_print_speed <= 0.0)
                throw std::runtime_error("Autospeed requires a positive max_print_speed.");

            double minimum = entry.second;
            // An enabled thin-flow floor replaces geometric sampling with a
            // configured flow derived from the active volumetric ceiling.
            if (thin_floor_enabled) {
                const double filament_limit = print_config.vector_float_or_default(
                    "filament_max_volumetric_speed", extruder_id, 0.0);
                const double flow_limit = filament_limit > 0.0 ?
                    std::min(maximum_volumetric, filament_limit) : maximum_volumetric;
                const double configured_floor = print_config.effective_float_or_percent_or_default(
                    "autospeed_min_thin_flow", flow_limit, 0.0);
                if (configured_floor > 0.0)
                    minimum = configured_floor / max_print_speed;
            }

            if (minimum != (std::numeric_limits<double>::max)() && minimum > 0.0)
                targets[group_idx][extruder_id] =
                    std::min(minimum * max_print_speed, maximum_volumetric);
        }
    }

    // Autospeed targets are now stable. Project every unresolved leaf's raw
    // regional tuples through the complete final-speed calculation and retain
    // only partitions that can actually produce distinct stored floats.
    std::set<SpeedPartitionKey> prepared_keys;
    for (uint32_t group_idx = 0; group_idx < plan.group_count(); ++group_idx) {
        const PrintingGroup group = plan.group(group_idx);
        for (uint32_t layer_idx = 0; layer_idx < group.layer_group_count(); ++layer_idx) {
            const PrintingLayerGroup layer_group = group.layer_group(layer_idx);
            for (uint32_t tool_idx = 0; tool_idx < layer_group.tool_group_count(); ++tool_idx) {
                const PrintingToolGroup tool_group = layer_group.tool_group(tool_idx);
                const double target = autospeed_target(targets, group_idx, tool_group.extruder_id());
                for (uint32_t extrusion_idx = 0; extrusion_idx < tool_group.extrusion_count(); ++extrusion_idx) {
                    const PrintingExtrusion extrusion = tool_group.extrusion(extrusion_idx);
                    const ExtrusionSettingsContext context = settings_context(print, tool_group, extrusion);
                    const RegionalSourceKey source{
                        extrusion.region_island().handle(), extrusion.object_instance_idx()
                    };
                    const RegionalSettingsPartitions::const_iterator settings_it =
                        regional_settings.find(source);
                    if (settings_it == regional_settings.end())
                        throw std::runtime_error("Validated speed source has no regional settings partition.");

                    collect_speed_partitions(
                        extrusion.mutable_root(), EffectiveTreeState{}, context, source,
                        settings_it->second, storage, group_idx, target, prepared_keys,
                        speed_partitions);
                }
            }
        }
    }
}

void collect_speed_partitions(MutableExtrusionEntity entity,
                              const EffectiveTreeState &parent_state,
                              const ExtrusionSettingsContext &context,
                              const RegionalSourceKey &source,
                              const RegionalSettingsPartition &regional_settings,
                              storage_handle *storage,
                              uint32_t group_idx,
                              double autospeed_target_value,
                              std::set<SpeedPartitionKey> &prepared_keys,
                              SpeedPartitions &speed_partitions)
{
    const EffectiveTreeState state = effective_state(entity, parent_state);
    if (entity.child_count() > 0) {
        for (uint32_t child_idx = 0; child_idx < entity.child_count(); ++child_idx) {
            collect_speed_partitions(
                entity.child_mutable(child_idx), state, context, source, regional_settings,
                storage, group_idx, autospeed_target_value, prepared_keys, speed_partitions);
        }
        return;
    }

    if (leaf_disposition(entity, state) != LeafDisposition::Editable || state.speed > 0.f)
        return;

    const raw_extrusion_role role = state.attributes.extrusion_role();
    const double raw_mm3_per_mm =
        state.attributes.c_extrusion_property_attributes::mm3_per_mm;
    const bool has_volumetric_flow =
        state.attributes.c_extrusion_property_attributes::height != -2.f &&
        raw_mm3_per_mm > 0.0;
    const double mm3_per_mm = has_volumetric_flow ? raw_mm3_per_mm : 0.0;
    const SpeedPartitionKey key{
        source, group_idx, context.extruder_id, role, state.full_overhang_speed,
        mm3_per_mm, has_volumetric_flow
    };
    if (!prepared_keys.insert(key).second)
        return;

    // Resolve every regional tuple with the same flow and autospeed inputs as
    // the future leaf. Equal final floats are united by the common projector.
    RegionalProcessPartition partition = project_regional_process_values(
        storage, regional_settings,
        [&context, role, &state, mm3_per_mm, has_volumetric_flow,
         autospeed_target_value](const Config &region_config) {
            ExtrusionSettingsContext regional_context = context;
            regional_context.region_config = region_config;
            return resolved_speed(
                regional_context, role, state.full_overhang_speed, mm3_per_mm,
                has_volumetric_flow, autospeed_target_value);
        });
    if (partition.requires_split())
        speed_partitions.emplace(key, std::move(partition));
}

void assign_speed_tree(MutableExtrusionEntity entity,
                       const EffectiveTreeState &parent_state,
                       const ExtrusionSettingsContext &context,
                       uint32_t group_idx,
                       const RegionalSourceKey &source,
                       double autospeed_target_value,
                       const SpeedPartitions &speed_partitions,
                       ProcessFieldEditor &editor)
{
    // Collections only propagate inherited state. Values are assigned to
    // leaves first so hoisting can later prove uniformity.
    const EffectiveTreeState state = effective_state(entity, parent_state);
    if (entity.child_count() > 0) {
        for (uint32_t child_idx = 0; child_idx < entity.child_count(); ++child_idx)
            assign_speed_tree(entity.child_mutable(child_idx), state, context,
                              group_idx, source, autospeed_target_value,
                              speed_partitions, editor);
        return;
    }

    const LeafDisposition disposition = leaf_disposition(entity, state);
    if (disposition == LeafDisposition::Empty || state.speed > 0.f)
        return;

    const raw_extrusion_role role = state.attributes.extrusion_role();
    // Travels are intentionally excluded from regional and autospeed setup,
    // but still need a concrete process speed before firmware serialization.
    if (RAW_EXTRUSION_ROLE_IS_TRAVEL(role)) {
        editor.set_value(entity, resolved_travel_speed(context));
        return;
    }
    if (disposition != LeafDisposition::Editable)
        return;

    const double raw_mm3_per_mm =
        state.attributes.c_extrusion_property_attributes::mm3_per_mm;
    const bool has_volumetric_flow =
        state.attributes.c_extrusion_property_attributes::height != -2.f &&
        raw_mm3_per_mm > 0.0;
    const double mm3_per_mm = has_volumetric_flow ? raw_mm3_per_mm : 0.0;
    const SpeedPartitionKey key{
        source, group_idx, context.extruder_id, role, state.full_overhang_speed,
        mm3_per_mm, has_volumetric_flow
    };
    const SpeedPartitions::const_iterator partition_it = speed_partitions.find(key);
    if (partition_it != speed_partitions.end()) {
        // The common splitter applies the already-final values to only the
        // regional fragments touched by this leaf.
        apply_regional_process_partition(entity, partition_it->second, editor);
        return;
    }

    editor.set_value(entity, resolved_speed(
        context, role, state.full_overhang_speed, mm3_per_mm,
        has_volumetric_flow, autospeed_target_value));
}

double autospeed_target(const AutospeedTargets &targets,
                        uint32_t group_idx,
                        uint16_t extruder_id)
{
    if (group_idx >= targets.size())
        return 0.0;
    const std::map<uint16_t, double>::const_iterator found = targets[group_idx].find(extruder_id);
    return found == targets[group_idx].end() ? 0.0 : found->second;
}

void edit_extrusion(const Print &print,
                    const PrintingToolGroup &tool_group,
                    const PrintingExtrusion &extrusion,
                    uint32_t group_idx,
                    double target,
                    const SpeedPartitions &speed_partitions)
{
    // Assign missing leaf speed, then remove redundant direct fields by moving
    // a uniform effective speed towards the root.
    const ExtrusionSettingsContext context = settings_context(print, tool_group, extrusion);
    const RegionalSourceKey source{
        extrusion.region_island().handle(), extrusion.object_instance_idx()
    };
    MutableExtrusionEntity root = extrusion.mutable_root();
    ProcessFieldEditor editor(ProcessField::Speed);
    assign_speed_tree(root, EffectiveTreeState{}, context, group_idx, source, target,
                      speed_partitions, editor);
    editor.hoist(root);
}

DefaultSpeed &DefaultSpeed::instance(orchestrator_handle *orch)
{
    static DefaultSpeed singleton(orch);
    return singleton;
}

DefaultSpeed::DefaultSpeed(orchestrator_handle *orch) : PluginBase(orch) {}

const char *DefaultSpeed::id_impl() const noexcept
{
    return "layer_extrusion_edit.speed.default";
}

const char *DefaultSpeed::name_impl() const noexcept
{
    return "Default extrusion speed";
}

const char *DefaultSpeed::description_impl() const noexcept
{
    return "Resolves missing extrusion speed, including volumetric autospeed.";
}

const char *DefaultSpeed::exclusive_group_impl() const noexcept
{
    return "layer_extrusion_edit.speed";
}

const char *DefaultSpeed::exclusive_group_label_impl() const noexcept
{
    return "Extrusion speed";
}

const char *DefaultSpeed::exclusive_group_tooltip_impl() const noexcept
{
    return "Selects how ordered layer extrusions receive their process speed.";
}

slicing_step_t DefaultSpeed::step_impl() const noexcept
{
    return STEP_LAYER_EXTRUSION_EDIT;
}

const char *const *DefaultSpeed::dependencies_impl() const noexcept
{
    return k_no_dependencies;
}

int32_t DefaultSpeed::priority_impl() const noexcept
{
    return 0;
}

int32_t DefaultSpeed::used_config_keys(raw_used_config_key *keys) const noexcept
{
    if (keys != nullptr)
        std::copy(std::begin(k_used_config_keys), std::end(k_used_config_keys), keys);
    return int32_t(std::size(k_used_config_keys));
}

const char *DefaultSpeed::progress_message_format_impl() const noexcept
{
    return "Resolving extrusion speed: %u / %u trees";
}

void DefaultSpeed::setup_impl(const plugin_run_context *run_ctx, uint32_t) const
{
    // Invalidate previous state first so a bad context cannot reuse targets
    // computed for an earlier printing plan.
    m_setup_valid = false;
    m_autospeed_targets.clear();
    m_speed_partitions.clear();

    const run_ctx_layer_extrusion_edition *ctx = plugin_ctx_as_layer_extrusion_edition(run_ctx);
    assert(ctx != nullptr && ctx->print != nullptr && ctx->plan != nullptr &&
           run_ctx != nullptr && run_ctx->plugin_storage != nullptr);
    if (ctx == nullptr || ctx->print == nullptr || ctx->plan == nullptr ||
        run_ctx == nullptr || run_ctx->plugin_storage == nullptr)
        return;

    prepare_plan(run_ctx->plugin_storage, Print(ctx->print), PrintingPlan(ctx->plan),
                 m_autospeed_targets, m_speed_partitions);
    m_setup_valid = true;
}

void DefaultSpeed::setup_run_impl(const plugin_run_context *run_ctx) const
{
    const run_ctx_layer_extrusion_edition *ctx = plugin_ctx_as_layer_extrusion_edition(run_ctx);
    if (ctx == nullptr || ctx->layer_group == nullptr)
        return;
    progress().add_max(extrusion_tree_count(PrintingLayerGroup(ctx->layer_group)));
}

void DefaultSpeed::run_impl(const plugin_run_context *run_ctx) const
{
    if (!m_setup_valid)
        return;
    const run_ctx_layer_extrusion_edition *ctx = plugin_ctx_as_layer_extrusion_edition(run_ctx);
    assert(ctx != nullptr && ctx->print != nullptr && ctx->layer_group != nullptr);
    if (ctx == nullptr || ctx->print == nullptr || ctx->layer_group == nullptr)
        return;

    // Each run owns one layer group and reads immutable targets prepared before
    // the host launched parallel work.
    const Print print(ctx->print);
    const PrintingLayerGroup layer_group(ctx->layer_group);
    for (uint32_t tool_idx = 0; tool_idx < layer_group.tool_group_count(); ++tool_idx) {
        const PrintingToolGroup tool_group = layer_group.tool_group(tool_idx);
        const double target = autospeed_target(m_autospeed_targets, ctx->group_idx, tool_group.extruder_id());
        for (uint32_t extrusion_idx = 0; extrusion_idx < tool_group.extrusion_count(); ++extrusion_idx) {
            edit_extrusion(print, tool_group, tool_group.extrusion(extrusion_idx),
                           ctx->group_idx, target, m_speed_partitions);
            progress().increment();
        }
    }
}

} // namespace

void register_default_speed_plugin(orchestrator_handle *orch)
{
    orchestrator_register_plugin(orch, DefaultSpeed::instance(orch).c_instance());
}

}}} // namespace slic3r_api::LayerExtrusionEdit::DefaultSpeedPlugin
