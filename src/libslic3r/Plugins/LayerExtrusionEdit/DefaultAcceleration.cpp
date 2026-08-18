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
#include <cstdint>
#include <iterator>

#include "ExtrusionProcessParameterHelpers.hpp"
#include "libslic3r/Api/plugin/c/steps/slic3r_step_layer_extrusion_edit.h"
#include "libslic3r/Api/plugin/cpp/PluginBase.hpp"

namespace slic3r_api { namespace LayerExtrusionEdit { namespace DefaultAccelerationPlugin {
namespace {

using namespace ProcessParameterHelpers;

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
    {"overhangs_acceleration", RAW_CO_FLOAT_OR_PERCENT, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE},
    {"perimeter_acceleration", RAW_CO_FLOAT_OR_PERCENT, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE},
    {"solid_infill_acceleration", RAW_CO_FLOAT_OR_PERCENT, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE},
    {"thin_walls_acceleration", RAW_CO_FLOAT_OR_PERCENT, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE},
    {"top_solid_infill_acceleration", RAW_CO_FLOAT_OR_PERCENT, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE}
};

// Resolve role acceleration, then apply first-layer and machine limits.
double role_acceleration(const ExtrusionSettingsContext &context,
                         raw_extrusion_role role);

// Validate all trees and source links before any parallel mutation begins.
void validate_plan(const Print &print, const PrintingPlan &plan);

// Assign missing acceleration recursively while preserving speed and overrides.
void assign_acceleration_tree(MutableExtrusionEntity entity,
                              const EffectiveTreeState &parent_state,
                              const ExtrusionSettingsContext &context,
                              ProcessFieldEditor &editor);

// Resolve and compact acceleration on one cloned extrusion root.
void edit_extrusion(const Print &print,
                    const PrintingToolGroup &tool_group,
                    const PrintingExtrusion &extrusion);

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

void validate_plan(const Print &print, const PrintingPlan &plan)
{
    // Visit every root before mutation. Acceleration needs valid attributes and
    // source configuration, but deliberately does not require volumetric flow.
    for (uint32_t group_idx = 0; group_idx < plan.group_count(); ++group_idx) {
        const PrintingGroup group = plan.group(group_idx);
        for (uint32_t layer_idx = 0; layer_idx < group.layer_group_count(); ++layer_idx) {
            const PrintingLayerGroup layer_group = group.layer_group(layer_idx);
            for (uint32_t tool_idx = 0; tool_idx < layer_group.tool_group_count(); ++tool_idx) {
                const PrintingToolGroup tool_group = layer_group.tool_group(tool_idx);
                for (uint32_t extrusion_idx = 0; extrusion_idx < tool_group.extrusion_count(); ++extrusion_idx)
                    (void)validated_settings_context(
                        print, tool_group, tool_group.extrusion(extrusion_idx), false);
            }
        }
    }
}

void assign_acceleration_tree(MutableExtrusionEntity entity,
                              const EffectiveTreeState &parent_state,
                              const ExtrusionSettingsContext &context,
                              ProcessFieldEditor &editor)
{
    // Collections propagate inherited state; only editable leaves receive a
    // missing acceleration value.
    const EffectiveTreeState state = effective_state(entity, parent_state);
    if (entity.child_count() > 0) {
        for (uint32_t child_idx = 0; child_idx < entity.child_count(); ++child_idx)
            assign_acceleration_tree(entity.child_mutable(child_idx), state, context, editor);
        return;
    }

    if (leaf_disposition(entity, state) != LeafDisposition::Editable || state.acceleration > 0.f)
        return;
    const double acceleration = role_acceleration(context, state.attributes.extrusion_role());
    editor.set_value(entity, float(acceleration));
}

void edit_extrusion(const Print &print,
                    const PrintingToolGroup &tool_group,
                    const PrintingExtrusion &extrusion)
{
    const ExtrusionSettingsContext context = settings_context(print, tool_group, extrusion);
    MutableExtrusionEntity root = extrusion.mutable_root();
    ProcessFieldEditor editor(ProcessField::Acceleration);
    assign_acceleration_tree(root, EffectiveTreeState{}, context, editor);
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
    const run_ctx_layer_extrusion_edition *ctx = plugin_ctx_as_layer_extrusion_edition(run_ctx);
    assert(ctx != nullptr && ctx->print != nullptr && ctx->plan != nullptr);
    if (ctx == nullptr || ctx->print == nullptr || ctx->plan == nullptr)
        return;

    validate_plan(Print(ctx->print), PrintingPlan(ctx->plan));
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
            edit_extrusion(print, tool_group, tool_group.extrusion(extrusion_idx));
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
