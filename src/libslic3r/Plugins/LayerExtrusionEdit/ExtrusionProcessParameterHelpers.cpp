///|/ Copyright (c) SuperSlicer 2026 Durand Remi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/

/*
Shared extrusion process-parameter helpers
==========================================

The helpers in this file separate tree mechanics from process-setting policy.
They resolve the properties inherited by each extrusion leaf, validate the
source links retained by PrintingPlan, and compact one selected process field
without changing geometry or ordering.

ProcessFieldEditor remembers which EPropertySpeed payloads it creates. This
allows it to remove a payload that becomes empty during hoisting while keeping
pre-existing payloads owned by another plugin intact.
*/

#include "ExtrusionProcessParameterHelpers.hpp"

#include <stdexcept>

namespace slic3r_api { namespace LayerExtrusionEdit { namespace ProcessParameterHelpers {

double effective_value(const Config &config, const char *key, double ratio)
{
    // Evaluate percentages against the inherited fallback, then keep that
    // fallback when the option is disabled or explicitly resolves to zero.
    const double value = config.effective_float_or_percent_or_default(key, ratio, ratio);
    return value > 0.0 ? value : ratio;
}

LeafDisposition leaf_disposition(const MutableExtrusionEntity &entity,
                                 const EffectiveTreeState &state)
{
    return leaf_disposition(entity, state, ProcessField::Speed);
}

LeafDisposition leaf_disposition(const MutableExtrusionEntity &entity,
                                 const EffectiveTreeState &state,
                                 ProcessField field)
{
    // Empty leaves do not emit G-code, while printable leaves need inherited
    // attributes before their role and flow can be interpreted safely.
    if (entity.point_count() == 0)
        return LeafDisposition::Empty;
    if (!state.has_attributes)
        throw std::runtime_error("Printable extrusion leaf has no effective EPropertyAttributes.");

    // These roles are valid printing-plan entities but fall outside the two
    // default process-parameter plugins, so their fields remain untouched.
    const raw_extrusion_role role = state.attributes.extrusion_role();
    if (role == RAW_EXTRUSION_ROLE_NONE || RAW_EXTRUSION_ROLE_IS_TRAVEL(role) ||
        RAW_EXTRUSION_ROLE_HAS(role, RAW_EXTRUSION_ROLE_WIPE_TOWER) ||
        (role == RAW_EXTRUSION_ROLE_MIXED && role != RAW_EXTRUSION_ROLE_GAP_FILL))
        return LeafDisposition::Excluded;

    // Cooling owns printable object paths, support, and skirt/brim. Milling
    // remains excluded because it is not controlled by filament fan settings.
    if (field == ProcessField::FanSpeed) {
        const bool fan_role = role == RAW_EXTRUSION_ROLE_GAP_FILL ||
                              role == RAW_EXTRUSION_ROLE_THIN_WALL ||
                              RAW_EXTRUSION_ROLE_IS_PERIMETER(role) ||
                              RAW_EXTRUSION_ROLE_IS_INFILL(role) ||
                              RAW_EXTRUSION_ROLE_IS_SKIRT(role) ||
                              RAW_EXTRUSION_ROLE_IS_SUPPORT(role);
        return fan_role && !RAW_EXTRUSION_ROLE_HAS(role, RAW_EXTRUSION_ROLE_MILL) ?
            LeafDisposition::Editable : LeafDisposition::Excluded;
    }

    if (RAW_EXTRUSION_ROLE_IS_SUPPORT(role))
        return LeafDisposition::Excluded;

    // Unknown future roles also remain untouched and prevent a property on an
    // ancestor from leaking into a subtree that the plugins do not understand.
    const bool known_role = role == RAW_EXTRUSION_ROLE_GAP_FILL ||
                            role == RAW_EXTRUSION_ROLE_THIN_WALL ||
                            RAW_EXTRUSION_ROLE_IS_PERIMETER(role) ||
                            RAW_EXTRUSION_ROLE_IS_INFILL(role) ||
                            RAW_EXTRUSION_ROLE_IS_SKIRT(role) ||
                            RAW_EXTRUSION_ROLE_HAS(role, RAW_EXTRUSION_ROLE_MILL);
    return known_role ? LeafDisposition::Editable : LeafDisposition::Excluded;
}

EffectiveTreeState effective_state(const MutableExtrusionEntity &entity,
                                   const EffectiveTreeState &parent_state)
{
    // Start from inherited values, then overlay only fields explicitly set on
    // this node. Negative process fields mean "inherit", not a real value.
    EffectiveTreeState state = parent_state;
    if (const EPropertyAttributes *attributes = entity.get(EPropertyAttributes::key)) {
        state.attributes = *attributes;
        state.has_attributes = true;
    }
    if (const EPropertySpeed *process = entity.get(EPropertySpeed::key)) {
        if (process->speed_mm_per_s > 0.f)
            state.speed = process->speed_mm_per_s;
        if (process->accel_mm_per_s2 > 0.f)
            state.acceleration = process->accel_mm_per_s2;
        if (process->fan_speed_percent >= 0.f)
            state.fan_speed = process->fan_speed_percent;
    }
    if (const EPropertyOverhang *overhang = entity.get(EPropertyOverhang::key)) {
        state.overhang = *overhang;
        state.has_overhang = true;
        state.full_overhang_speed = overhang->has_full_overhangs_speed != 0;
    }
    return state;
}

ExtrusionSettingsContext settings_context(const Print &print,
                                          const PrintingToolGroup &tool_group,
                                          const PrintingExtrusion &extrusion)
{
    // The ordered plan keeps a link to its source region-island. Follow that
    // link to recover the configuration scopes that govern the cloned tree.
    const LayerRegionIsland region_island = extrusion.region_island();
    if (!region_island.valid() || region_island.region_count() == 0)
        throw std::runtime_error("Printing extrusion has no source LayerRegionIsland region.");
    if (tool_group.extruder_id() == uint16_t(-1))
        throw std::runtime_error("Printable extrusion has no concrete extruder id.");

    const LayerRegion layer_region = region_island.region(0);
    const Layer layer = layer_region.layer();
    const Object object = layer.object();

    // Keep the three scopes separate because each process rule is owned by a
    // different level of the print configuration.
    ExtrusionSettingsContext context = {};
    context.print_config = print.config();
    context.object_config = object.config();
    context.region_config = layer_region.print_region().config();
    context.first_layer = layer.bottom_z() <= 0;
    context.extruder_id = tool_group.extruder_id();
    return context;
}

void validate_tree(const MutableExtrusionEntity &entity,
                   const EffectiveTreeState &parent_state,
                   bool require_volumetric_flow)
{
    // Resolve inherited properties while descending so validation observes the
    // same effective state as the later mutation pass.
    const EffectiveTreeState state = effective_state(entity, parent_state);
    if (entity.child_count() > 0) {
        for (uint32_t child_idx = 0; child_idx < entity.child_count(); ++child_idx)
            validate_tree(entity.child_mutable(child_idx), state, require_volumetric_flow);
        return;
    }

    if (leaf_disposition(entity, state) != LeafDisposition::Editable || !require_volumetric_flow)
        return;

    // Normal extrusion speed needs a real volumetric flow. Milling and
    // force-e commands express material differently and are valid without it.
    const raw_extrusion_role role = state.attributes.extrusion_role();
    const bool force_e_per_mm = state.attributes.c_extrusion_property_attributes::height == -2.f;
    if (!force_e_per_mm && !RAW_EXTRUSION_ROLE_HAS(role, RAW_EXTRUSION_ROLE_MILL) &&
        state.attributes.c_extrusion_property_attributes::mm3_per_mm <= 0.0)
        throw std::runtime_error("Printable extrusion leaf has no positive volumetric flow.");
}

ExtrusionSettingsContext validated_settings_context(const Print &print,
                                                    const PrintingToolGroup &tool_group,
                                                    const PrintingExtrusion &extrusion,
                                                    bool require_volumetric_flow)
{
    // Validate both tree properties and source links before returning a context
    // that a plugin may safely use for mutation.
    validate_tree(extrusion.mutable_root(), EffectiveTreeState{}, require_volumetric_flow);
    return settings_context(print, tool_group, extrusion);
}

uint32_t extrusion_tree_count(const PrintingLayerGroup &layer_group)
{
    uint32_t tree_count = 0;
    for (uint32_t tool_idx = 0; tool_idx < layer_group.tool_group_count(); ++tool_idx)
        tree_count += layer_group.tool_group(tool_idx).extrusion_count();
    return tree_count;
}

ProcessFieldEditor::ProcessFieldEditor(ProcessField field) : m_field(field) {}

float ProcessFieldEditor::state_value(const EffectiveTreeState &state) const
{
    switch (m_field) {
    case ProcessField::Speed: return state.speed;
    case ProcessField::Acceleration: return state.acceleration;
    case ProcessField::FanSpeed: return state.fan_speed;
    }
    return -1.f;
}

bool ProcessFieldEditor::value_is_set(float value) const
{
    return m_field == ProcessField::FanSpeed ? value >= 0.f : value > 0.f;
}

float ProcessFieldEditor::direct_value(const EPropertySpeed *process) const
{
    if (process == nullptr)
        return -1.f;
    switch (m_field) {
    case ProcessField::Speed: return process->speed_mm_per_s;
    case ProcessField::Acceleration: return process->accel_mm_per_s2;
    case ProcessField::FanSpeed: return process->fan_speed_percent;
    }
    return -1.f;
}

float &ProcessFieldEditor::field(EPropertySpeed &process) const
{
    switch (m_field) {
    case ProcessField::Speed: return process.speed_mm_per_s;
    case ProcessField::Acceleration: return process.accel_mm_per_s2;
    case ProcessField::FanSpeed: return process.fan_speed_percent;
    }
    return process.speed_mm_per_s;
}

EPropertySpeed &ProcessFieldEditor::ensure_property(MutableExtrusionEntity entity)
{
    // Reuse pre-existing metadata so pressure, fan, temperature, and the other
    // process field survive when this editor writes its selected field.
    if (EPropertySpeed *existing = entity.get_mutable(EPropertySpeed::key))
        return *existing;

    // A new C payload is zero-initialized, but zero would look like a concrete
    // optional value. Initialize every field to the explicit unset sentinel.
    EPropertySpeed &created = entity.get_or_add(EPropertySpeed::key);
    created.speed_mm_per_s = -1.f;
    created.accel_mm_per_s2 = -1.f;
    created.pressure_adv = -1.f;
    created.fan_speed_percent = -1.f;
    created.temperature_C = -1.f;
    m_created_properties.insert(entity.mutable_handle());
    return created;
}

void ProcessFieldEditor::set_value(MutableExtrusionEntity entity, float value)
{
    if (!value_is_set(value))
        return;
    field(ensure_property(entity)) = value;
    m_modified_entities.insert(entity.mutable_handle());
}

ProcessFieldEditor::FieldSummary ProcessFieldEditor::merge_fields(
    const std::vector<FieldSummary> &fields) const
{
    // A parent may inherit one value only if every relevant child agrees. A
    // blocked child or two distinct values makes that field unsafe to hoist.
    FieldSummary merged = {};
    for (const FieldSummary &child : fields) {
        merged.blocked = merged.blocked || child.blocked;
        if (!child.has_value)
            continue;
        if (!merged.has_value) {
            merged.has_value = true;
            merged.value = child.value;
        } else if (merged.value != child.value) {
            merged.blocked = true;
        }
    }
    return merged;
}

void ProcessFieldEditor::clear_redundant_field(MutableExtrusionEntity entity,
                                               float inherited_value)
{
    // Remove only a direct field that exactly duplicates the newly proven
    // inherited value. Every unrelated process field remains untouched.
    EPropertySpeed *process = entity.get_mutable(EPropertySpeed::key);
    if (process == nullptr)
        return;
    // A coincidentally equal upstream fan override must remain physically
    // present. Otherwise a later rerun could remove the generated parent and
    // silently lose the producer's original intent.
    if (m_field == ProcessField::FanSpeed &&
        m_modified_entities.count(entity.mutable_handle()) == 0)
        return;
    float &direct_field = field(*process);
    if (!value_is_set(direct_field) || direct_field != inherited_value)
        return;
    direct_field = -1.f;
    m_modified_entities.erase(entity.mutable_handle());

    // Delete an empty payload only when this editor created it. Pre-existing
    // payloads remain owned by the plugin or producer that supplied them.
    const bool empty = process->speed_mm_per_s < 0.f && process->accel_mm_per_s2 < 0.f &&
        process->pressure_adv < 0.f && process->fan_speed_percent < 0.f && process->temperature_C < 0.f;
    if (empty && m_created_properties.erase(entity.mutable_handle()) > 0)
        entity.remove(EPropertySpeed::key);
}

ProcessFieldEditor::FieldSummary ProcessFieldEditor::hoist_tree(
    MutableExtrusionEntity entity,
    const EffectiveTreeState &parent_state)
{
    // Leaves report their effective selected field upward. Excluded leaves
    // block hoisting so a parent value cannot change their behavior.
    EffectiveTreeState state = effective_state(entity, parent_state);
    if (entity.child_count() == 0) {
        const LeafDisposition disposition = leaf_disposition(entity, state, m_field);
        if (disposition == LeafDisposition::Empty)
            return {};
        if (disposition == LeafDisposition::Excluded)
            return FieldSummary{false, true, -1.f};
        const float value = state_value(state);
        return FieldSummary{value_is_set(value), !value_is_set(value), value};
    }

    // Process children first so the parent receives a proof about all of its
    // printable descendants rather than only its direct children.
    std::vector<FieldSummary> child_summaries;
    child_summaries.reserve(entity.child_count());
    for (uint32_t child_idx = 0; child_idx < entity.child_count(); ++child_idx)
        child_summaries.push_back(hoist_tree(entity.child_mutable(child_idx), state));
    const FieldSummary summary = merge_fields(child_summaries);

    // A conflicting direct value is authoritative. Otherwise store a uniform
    // descendant value here and clear matching direct values from children.
    EPropertySpeed *direct_process = entity.get_mutable(EPropertySpeed::key);
    const float direct = direct_value(direct_process);
    if (summary.has_value && !summary.blocked && (!value_is_set(direct) || direct == summary.value)) {
        if (!value_is_set(direct) && state_value(state) != summary.value) {
            field(ensure_property(entity)) = summary.value;
            m_modified_entities.insert(entity.mutable_handle());
            if (m_field == ProcessField::Speed)
                state.speed = summary.value;
            else if (m_field == ProcessField::Acceleration)
                state.acceleration = summary.value;
            else
                state.fan_speed = summary.value;
        }
        const float inherited = value_is_set(direct) ? direct :
            (value_is_set(state_value(state)) ? state_value(state) : summary.value);
        if (inherited == summary.value)
            for (uint32_t child_idx = 0; child_idx < entity.child_count(); ++child_idx)
                if (child_summaries[child_idx].has_value && !child_summaries[child_idx].blocked)
                    clear_redundant_field(entity.child_mutable(child_idx), inherited);
    }
    return summary;
}

void ProcessFieldEditor::hoist(MutableExtrusionEntity root)
{
    (void)hoist_tree(root, EffectiveTreeState{});
}

}}} // namespace slic3r_api::LayerExtrusionEdit::ProcessParameterHelpers
