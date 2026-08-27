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

The ordered PrintingPlan contains explicit movement leaves. This plugin walks
those leaves in the same hierarchy order as the G-code writer and stores one
FEATURE_GCODE script whenever the effective EPropertyAttributes role changes.
Explicit travels participate because travel is a normal role in the new
extrusion stream; synthetic firmware travels are not present in the plan.

The script normally lives directly on the movement leaf. If that leaf already
owns another custom G-code property, the leaf is moved below a new parent and
the parent carries the feature script. The firmware processes a node's script
before its children, so both shapes preserve the same execution order.
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
// A generated wrapper has no local information besides one feature script and
// one child containing the original entity.
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

    storage_handle *m_storage;
    const std::string &m_script;
    StoredConfig m_arguments;
    std::optional<raw_extrusion_role> m_previous_role;
};

std::string script_role_name(raw_extrusion_role raw_role)
{
    constexpr uint32_t known_mask = (uint32_t(1) << 13) - 1;
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
    // that base, but unknown bits or two simultaneous bases are malformed.
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
    const EPropertyCustomGcode *property = entity.property<EPropertyCustomGcode>();
    return property != nullptr &&
           property->kind == C_EXTRUSION_CUSTOM_GCODE_SCRIPT &&
           property->script_type == GCODE_SCRIPT_TYPE_FEATURE_GCODE;
}

bool is_feature_wrapper(const MutableExtrusionEntity &entity)
{
    return has_feature_script(entity) && entity.property_count() == 1 && entity.child_count() == 1;
}

void clear_feature_scripts(storage_handle *storage, MutableExtrusionEntity root)
{
    std::vector<MutableExtrusionEntity> pending;
    pending.push_back(root);

    while (!pending.empty()) {
        MutableExtrusionEntity entity = pending.back();
        pending.pop_back();

        // Repeatedly collapse generated wrappers so rerunning the step never
        // accumulates transparent parent levels around one movement.
        while (is_feature_wrapper(entity)) {
            StoredExtrusionEntity child(storage);
            if (!child.move_from(entity.child_mutable(0)) ||
                extrusion_move_from(entity.mutable_handle(), child.mutable_handle()) == 0)
                throw std::runtime_error("Failed to remove a generated feature G-code wrapper.");
        }

        if (has_feature_script(entity) && !entity.remove_property<EPropertyCustomGcode>())
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
    : m_storage(storage)
    , m_script(script)
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

    const EPropertyAttributes *attributes = current_property<EPropertyAttributes>();
    if (attributes == nullptr)
        return;

    const raw_extrusion_role current_role = attributes->extrusion_role();
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

    if (!entity.has_property<EPropertyCustomGcode>()) {
        entity.script_gcode(m_script, GCODE_SCRIPT_TYPE_FEATURE_GCODE, m_arguments);
        return;
    }

    /*
    One entity stores at most one property of each type. Moving the complete
    leaf into an owned temporary preserves its geometry, properties and data
    resources. Moving that temporary below the now-empty original handle turns
    the stable plan handle into the required parent node.
    */
    StoredExtrusionEntity original(m_storage);
    if (!original.move_from(entity))
        throw std::runtime_error("Failed to preserve an extrusion before adding feature G-code.");
    entity.script_gcode(m_script, GCODE_SCRIPT_TYPE_FEATURE_GCODE, m_arguments);
    if (is_invalid_index(entity.append_child_move(original.mutable_view())))
        throw std::runtime_error("Failed to wrap an extrusion carrying custom G-code.");
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
