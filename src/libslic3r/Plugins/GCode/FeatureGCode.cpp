///|/ Copyright (c) SuperSlicer 2026 Durand Remi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/

#include "FeatureGCode.hpp"

#include <cassert>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include <boost/log/trivial.hpp>

#include "libslic3r/Api/plugin/c/steps/slic3r_step_extrusion_edit.h"
#include "libslic3r/Api/plugin/cpp/ConfigViews.hpp"
#include "libslic3r/Api/plugin/cpp/DataTreeViews.hpp"
#include "libslic3r/Api/plugin/cpp/ExtrusionTreeVisitors.hpp"
#include "libslic3r/Api/plugin/cpp/PluginBase.hpp"
#include "libslic3r/Api/plugin/cpp/PrintingPlanViews.hpp"
#include "libslic3r/ExtrusionRole.hpp"

/*
Feature G-code role annotations
===============================

This plugin places the configured `feature_gcode` script at each transition
between effective extrusion roles in the ordered PrintingPlan. It runs at
STEP_EXTRUSION_EDIT and prepares role-change events for the final G-code
writer; it does not emit machine G-code itself.

The normal execution flow is:

    run_impl()
    |-- remove feature scripts and generated wrappers from an earlier run
    |-- create one FeatureRoleVisitor with a reusable script Config
    |-- traverse every tool group's extrusion tree in plan order
    |   |-- ignore empty leaves and retraction/wipe/unretraction roles
    |   |-- read the effective EPropertyAttributes role
    |   |-- compare it with the previous printable role
    |   `-- insert a feature event when the role changes
    `-- leave the plan unchanged when `feature_gcode` is empty

When a movement leaf has no custom-G-code property, the script is attached
directly to that leaf. If another custom-G-code property is already present, a
non-geometric ordered leaf is inserted immediately before the movement and
the original entity remains intact below the wrapper. The script receives the
previous and next role names through a Config snapshot, including both
`previous_extrusion_role`/`last_extrusion_role` and
`next_extrusion_role`/`extrusion_role` for the supported template vocabulary.

Role validation requires one base role and permits its modifiers. Roles that
are structurally valid but have no representable feature-G-code name are
reported and mapped to `Custom`. Explicit travel leaves participate in role
transitions; synthetic travel created later by the G-code writer is outside
this plan traversal. Cleanup recognizes only the wrappers produced by this
plugin, so pre-existing custom-G-code properties are preserved.
*/

namespace slic3r_api { namespace GCodeGeneration { namespace FeatureGCodePlugin {
namespace {

const char *const k_no_dependencies[] = {nullptr};

// Convert a valid extrusion role to the stable text consumed by feature_gcode.
// Structurally valid roles which the G-code vocabulary cannot express are
// deliberately represented as Custom instead of being silently mislabeled.
std::string script_role_name(raw_extrusion_role raw_role);
// Return true only for a direct script property owned by this built-in plugin.
bool has_feature_script(const MutableExtrusionEntity &entity);
// A generated wrapper has no direct information and starts with one feature
// event leaf followed by the complete original entity.
bool is_feature_wrapper(const MutableExtrusionEntity &entity);
// Remove annotations from an earlier run and restore wrapped entities without
// copying their geometry or stored property data.
void clear_feature_scripts(storage_handle *storage, MutableExtrusionEntity root);
// Clear every cloned extrusion tree before recomputing transitions.
void clear_plan_feature_scripts(storage_handle *storage, const PrintingPlan &plan);
// Annotate every role transition while preserving one previous role across the
// complete ordered plan.
void add_plan_feature_scripts(storage_handle *storage,
                              const PrintingPlan &plan,
                              const std::string &script);

class FeatureRoleVisitor final : public ExtrusionTreeVisitor<>
{
public:
    FeatureRoleVisitor(storage_handle *storage, const std::string &script);

protected:
    void visit_leaf(MutableExtrusionEntity entity) override;

private:
    // Attach the feature property directly when possible. A conflicting
    // custom-G-code property is preserved below a new wrapper node.
    void add_transition(MutableExtrusionEntity entity,
                        raw_extrusion_role previous_role,
                        raw_extrusion_role next_role);
    // Update the reusable producer Config before script_gcode() snapshots it.
    void set_role_arguments(const std::string &previous_role,
                            const std::string &next_role);

    const std::string &m_script;
    StoredConfig m_arguments;
    std::optional<raw_extrusion_role> m_previous_role;
};

std::string script_role_name(raw_extrusion_role raw_role)
{
    constexpr uint32_t known_mask = (uint32_t(1) << 16) - 1;
    constexpr uint32_t base_mask =
        uint32_t(RAW_EXTRUSION_ROLE_PERIMETER) |
        uint32_t(RAW_EXTRUSION_ROLE_INFILL) |
        uint32_t(RAW_EXTRUSION_ROLE_SUPPORT) |
        uint32_t(RAW_EXTRUSION_ROLE_SKIRT) |
        uint32_t(RAW_EXTRUSION_ROLE_WIPE_TOWER) |
        uint32_t(RAW_EXTRUSION_ROLE_MILL) |
        uint32_t(RAW_EXTRUSION_ROLE_MIXED) |
        uint32_t(RAW_EXTRUSION_ROLE_TRAVEL);
    const uint32_t bits = static_cast<uint32_t>(raw_role);
    const uint32_t base = bits & base_mask;

    // A non-empty role has exactly one base bit. Modifiers may then refine
    // that base. Process-only roles are filtered before this conversion, and
    // two simultaneous base roles remain malformed.
    if ((bits & ~known_mask) != 0 ||
        (bits != 0 && (base == 0 || (base & (base - 1)) != 0)))
        throw std::invalid_argument("A feature G-code context contains an invalid extrusion role.");

    const Slic3r::ExtrusionRole role{
        static_cast<Slic3r::ExtrusionRoleModifier>(raw_role)};
    const bool supported =
        role == Slic3r::ExtrusionRole::None ||
        role == Slic3r::ExtrusionRole::Perimeter ||
        role == Slic3r::ExtrusionRole::ExternalPerimeter ||
        role == Slic3r::ExtrusionRole::OverhangPerimeter ||
        role == Slic3r::ExtrusionRole::OverhangExternalPerimeter ||
        role == Slic3r::ExtrusionRole::InternalInfill ||
        role == Slic3r::ExtrusionRole::SolidInfill ||
        role == Slic3r::ExtrusionRole::TopSolidInfill ||
        role == Slic3r::ExtrusionRole::Ironing ||
        role == Slic3r::ExtrusionRole::BridgeInfill ||
        role == Slic3r::ExtrusionRole::InternalBridgeInfill ||
        role == Slic3r::ExtrusionRole::ThinWall ||
        role == Slic3r::ExtrusionRole::GapFill ||
        role == Slic3r::ExtrusionRole::Skirt ||
        role == Slic3r::ExtrusionRole::SupportMaterial ||
        role == Slic3r::ExtrusionRole::SupportMaterialInterface ||
        role == Slic3r::ExtrusionRole::WipeTower ||
        role == Slic3r::ExtrusionRole::WipeTowerRamming ||
        role == Slic3r::ExtrusionRole::WipeTowerWipe ||
        role == Slic3r::ExtrusionRole::Milling ||
        role == Slic3r::ExtrusionRole::Travel;
    if (!supported) {
        BOOST_LOG_TRIVIAL(warning)
            << "Unsupported extrusion role " << raw_role
            << " in a feature G-code context; using Custom.";
        return Slic3r::gcode_extrusion_role_to_string(
            Slic3r::GCodeExtrusionRole::Custom);
    }
    return Slic3r::er_to_string(role);
}

bool has_feature_script(const MutableExtrusionEntity &entity)
{
    const EPropertyCustomGcode *property = entity.get(EPropertyCustomGcode::key);
    return property != nullptr &&
           property->kind == C_EXTRUSION_CUSTOM_GCODE_SCRIPT &&
           property->script_type == GCODE_SCRIPT_TYPE_FEATURE_GCODE;
}

bool is_feature_wrapper(const MutableExtrusionEntity &entity)
{
    if (entity.property_count() != 0 || entity.child_count() != 2)
        return false;

    const MutableExtrusionEntity event = entity.child_mutable(0);
    return has_feature_script(event) && event.property_count() == 1 && event.empty();
}

void clear_feature_scripts(storage_handle *storage, MutableExtrusionEntity root)
{
    std::vector<MutableExtrusionEntity> pending;
    pending.push_back(root);

    while (!pending.empty()) {
        MutableExtrusionEntity entity = pending.back();
        pending.pop_back();

        // Repeatedly remove the generated event leaf and move the preserved
        // original entity back into its stable plan handle.
        while (is_feature_wrapper(entity)) {
            StoredExtrusionEntity original(storage);
            if (!original.move_from(entity.child_mutable(1)) ||
                extrusion_move_from(entity.mutable_handle(), original.mutable_handle()) == 0)
                throw std::runtime_error("Failed to remove a generated feature G-code wrapper.");
        }

        if (has_feature_script(entity) && !entity.remove(EPropertyCustomGcode::key))
            throw std::runtime_error("Failed to remove a generated feature G-code property.");

        // Push in reverse so inspection still follows normal child order even
        // though cleanup itself does not depend on traversal order.
        for (uint32_t child_idx = entity.child_count(); child_idx > 0; --child_idx)
            pending.push_back(entity.child_mutable(child_idx - 1));
    }
}

void clear_plan_feature_scripts(storage_handle *storage, const PrintingPlan &plan)
{
    for (uint32_t group_idx = 0; group_idx < plan.group_count(); ++group_idx) {
        const PrintingGroup group = plan.group(group_idx);
        for (uint32_t layer_idx = 0; layer_idx < group.layer_group_count(); ++layer_idx) {
            const PrintingLayerGroup layer = group.layer_group(layer_idx);
            for (uint32_t tool_idx = 0; tool_idx < layer.tool_group_count(); ++tool_idx) {
                const PrintingToolGroup tool_group = layer.tool_group(tool_idx);
                for (uint32_t extrusion_idx = 0; extrusion_idx < tool_group.extrusion_count(); ++extrusion_idx)
                    clear_feature_scripts(storage, tool_group.extrusion(extrusion_idx).mutable_root());
            }
        }
    }
}

FeatureRoleVisitor::FeatureRoleVisitor(storage_handle *storage, const std::string &script)
    : m_script(script)
    , m_arguments(storage)
{
    // Create the schema once. Each transition only replaces the four string
    // values before script_gcode() serializes an immutable snapshot.
    m_arguments.get_or_add("previous_extrusion_role", SLIC3R_CONFIG_OPTION_STRING).set_string(std::string());
    m_arguments.get_or_add("next_extrusion_role", SLIC3R_CONFIG_OPTION_STRING).set_string(std::string());
    m_arguments.get_or_add("last_extrusion_role", SLIC3R_CONFIG_OPTION_STRING).set_string(std::string());
    m_arguments.get_or_add("extrusion_role", SLIC3R_CONFIG_OPTION_STRING).set_string(std::string());
}

void FeatureRoleVisitor::visit_leaf(MutableExtrusionEntity entity)
{
    if (entity.segment_count() == 0)
        return;

    const EPropertyAttributes *attributes = current_property(EPropertyAttributes::key);
    if (attributes == nullptr)
        return;

    const raw_extrusion_role current_role = attributes->extrusion_role();
    // Retraction, wipe and unretraction describe machine preparation around a
    // travel. They must not interrupt the last printable feature remembered by
    // feature_gcode or create user-visible feature transitions of their own.
    if (RAW_EXTRUSION_ROLE_IS_WIPE(current_role) ||
        RAW_EXTRUSION_ROLE_IS_RETRACT(current_role) ||
        RAW_EXTRUSION_ROLE_IS_UNRETRACT(current_role))
        return;
    if (!m_previous_role || *m_previous_role != current_role)
        add_transition(entity, m_previous_role.value_or(RAW_EXTRUSION_ROLE_NONE), current_role);
    m_previous_role = current_role;
}

void FeatureRoleVisitor::set_role_arguments(const std::string &previous_role,
                                            const std::string &next_role)
{
    m_arguments.get_mutable("previous_extrusion_role").set_string(previous_role);
    m_arguments.get_mutable("next_extrusion_role").set_string(next_role);
    m_arguments.get_mutable("last_extrusion_role").set_string(previous_role);
    m_arguments.get_mutable("extrusion_role").set_string(next_role);
}

void FeatureRoleVisitor::add_transition(MutableExtrusionEntity entity,
                                        raw_extrusion_role previous_role,
                                        raw_extrusion_role next_role)
{
    set_role_arguments(script_role_name(previous_role), script_role_name(next_role));

    if (!entity.has(EPropertyCustomGcode::key)) {
        entity.script_gcode(m_script, GCODE_SCRIPT_TYPE_FEATURE_GCODE, m_arguments);
        return;
    }

    MutableExtrusionEntity event = entity.emplace_ordered_leaf(
        OrderedLeafPosition::Before,
        ExistingPropertyPlacement::MoveWithExistingContent);
    if (!event.valid())
        throw std::runtime_error("Failed to insert feature G-code before an extrusion.");
    event.script_gcode(m_script, GCODE_SCRIPT_TYPE_FEATURE_GCODE, m_arguments);
}

void add_plan_feature_scripts(storage_handle *storage,
                              const PrintingPlan &plan,
                              const std::string &script)
{
    FeatureRoleVisitor visitor(storage, script);
    for (uint32_t group_idx = 0; group_idx < plan.group_count(); ++group_idx) {
        const PrintingGroup group = plan.group(group_idx);
        for (uint32_t layer_idx = 0; layer_idx < group.layer_group_count(); ++layer_idx) {
            const PrintingLayerGroup layer = group.layer_group(layer_idx);
            for (uint32_t tool_idx = 0; tool_idx < layer.tool_group_count(); ++tool_idx) {
                const PrintingToolGroup tool_group = layer.tool_group(tool_idx);
                for (uint32_t extrusion_idx = 0; extrusion_idx < tool_group.extrusion_count(); ++extrusion_idx)
                    visitor.traverse(tool_group.extrusion(extrusion_idx).mutable_root());
            }
        }
    }
}

class FeatureGCode final : public PluginBase
{
public:
    static FeatureGCode &instance(orchestrator_handle *orchestrator)
    {
        static FeatureGCode plugin(orchestrator);
        return plugin;
    }

    explicit FeatureGCode(orchestrator_handle *orchestrator) : PluginBase(orchestrator) {}

private:
    const char *id_impl() const noexcept override { return "gcode.feature_gcode"; }
    const char *name_impl() const noexcept override { return "Feature G-code"; }
    const char *description_impl() const noexcept override
    {
        return "Places the configured feature script at every ordered extrusion-role transition.";
    }
    slicing_step_t step_impl() const noexcept override { return STEP_EXTRUSION_EDIT; }
    const char *const *dependencies_impl() const noexcept override { return k_no_dependencies; }
    int32_t priority_impl() const noexcept override { return 1000; }
    int32_t used_config_keys(raw_used_config_key *keys) const noexcept override
    {
        if (keys != nullptr)
            keys[0] = raw_used_config_key{
                "feature_gcode", RAW_CO_STRING, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE};
        return 1;
    }
    void run_impl(const plugin_run_context *run_context) const override
    {
        const run_ctx_extrusion_edition *context = plugin_ctx_as_extrusion_edition(run_context);
        assert(context != nullptr && context->print != nullptr && context->plan != nullptr);
        if (context == nullptr || context->print == nullptr || context->plan == nullptr ||
            run_context->plugin_storage == nullptr)
            return;

        const Print print(context->print);
        const PrintingPlan plan(context->plan);
        clear_plan_feature_scripts(run_context->plugin_storage, plan);

        const std::string script = print.config().string_or_default("feature_gcode", std::string());
        if (!script.empty())
            add_plan_feature_scripts(run_context->plugin_storage, plan, script);
    }
};

} // namespace

void register_feature_gcode_plugin(orchestrator_handle *orchestrator)
{
    orchestrator_register_plugin(orchestrator, FeatureGCode::instance(orchestrator).c_instance());
}

}}} // namespace slic3r_api::GCodeGeneration::FeatureGCodePlugin
