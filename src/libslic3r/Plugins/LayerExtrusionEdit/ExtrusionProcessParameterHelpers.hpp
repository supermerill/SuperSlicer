///|/ Copyright (c) SuperSlicer 2026 Durand Remi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/

#ifndef slic3r_Plugins_LayerExtrusionEdit_ExtrusionProcessParameterHelpers_hpp_
#define slic3r_Plugins_LayerExtrusionEdit_ExtrusionProcessParameterHelpers_hpp_

#include <cstdint>
#include <set>
#include <vector>

#include "libslic3r/Api/plugin/cpp/ConfigViews.hpp"
#include "libslic3r/Api/plugin/cpp/PrintingPlanViews.hpp"

/*
Shared extrusion process-parameter helpers
==========================================

STEP_LAYER_EXTRUSION_EDIT plugins annotate cloned PrintingPlan extrusion trees.
Speed and acceleration use different business rules, but they both need to
resolve inherited properties, recover source configuration, preserve unrelated
process fields, and compact uniform values towards collection nodes.

This module provides those structural operations without choosing a speed or
acceleration value. Each plugin owns its validation and setting hierarchy, then
uses ProcessFieldEditor to write and hoist only its assigned field.
*/

namespace slic3r_api { namespace LayerExtrusionEdit { namespace ProcessParameterHelpers {

enum class LeafDisposition
{
    Empty,
    Editable,
    Excluded
};

enum class ProcessField
{
    Speed,
    Acceleration,
    FanSpeed
};

struct EffectiveTreeState
{
    EPropertyAttributes attributes = {};
    bool has_attributes = false;
    float speed = -1.f;
    float acceleration = -1.f;
    float fan_speed = -1.f;
    EPropertyOverhang overhang = {};
    bool has_overhang = false;
    bool full_overhang_speed = false;
};

struct ExtrusionSettingsContext
{
    Config print_config;
    Config object_config;
    Config region_config;
    bool first_layer = false;
    uint16_t extruder_id = uint16_t(-1);
};

// Resolve one positive override against a caller-provided base. Missing,
// disabled, and zero options inherit that base.
double effective_value(const Config &config, const char *key, double ratio);

// Classify a printable leaf. Excluded leaves are deliberately left untouched
// and block property hoisting through their parent subtree.
LeafDisposition leaf_disposition(const MutableExtrusionEntity &entity,
                                 const EffectiveTreeState &state);

// Classify leaves for one process field. Fan editing includes support and
// skirt/brim, while speed and acceleration retain their narrower role set.
LeafDisposition leaf_disposition(const MutableExtrusionEntity &entity,
                                 const EffectiveTreeState &state,
                                 ProcessField field);

// Overlay a node's direct attributes and process fields on inherited state.
EffectiveTreeState effective_state(const MutableExtrusionEntity &entity,
                                   const EffectiveTreeState &parent_state);

// Recover the print, object, region, layer, and extruder context linked from a
// cloned PrintingExtrusion.
ExtrusionSettingsContext settings_context(const Print &print,
                                          const PrintingToolGroup &tool_group,
                                          const PrintingExtrusion &extrusion);

// Validate attributes throughout one tree. Speed callers request volumetric
// flow validation; acceleration callers only require usable role attributes.
void validate_tree(const MutableExtrusionEntity &entity,
                   const EffectiveTreeState &parent_state,
                   bool require_volumetric_flow);

// Validate one tree and its source configuration before a plugin mutates it.
ExtrusionSettingsContext validated_settings_context(const Print &print,
                                                    const PrintingToolGroup &tool_group,
                                                    const PrintingExtrusion &extrusion,
                                                    bool require_volumetric_flow);

// Count the independently edited extrusion roots in one layer group.
uint32_t extrusion_tree_count(const PrintingLayerGroup &layer_group);

class ProcessFieldEditor
{
public:
    explicit ProcessFieldEditor(ProcessField field);

    ProcessFieldEditor(const ProcessFieldEditor &) = delete;
    ProcessFieldEditor &operator=(const ProcessFieldEditor &) = delete;

    // Set a direct value while preserving every other process field. Speed
    // and acceleration require a positive value; fan speed also accepts zero.
    void set_value(MutableExtrusionEntity entity, float value);

    // Move a uniform effective value towards the root without crossing an
    // excluded, unresolved, or conflicting subtree.
    void hoist(MutableExtrusionEntity root);

    // Handles in this set still carry a direct field created by this editor
    // after hoisting. They remain borrowed from the edited extrusion tree.
    const std::set<extrusion_entity_handle *> &modified_entities() const {
        return m_modified_entities;
    }

private:
    struct FieldSummary
    {
        bool has_value = false;
        bool blocked = false;
        float value = -1.f;
    };

    float state_value(const EffectiveTreeState &state) const;
    bool value_is_set(float value) const;
    float direct_value(const EPropertySpeed *process) const;
    float &field(EPropertySpeed &process) const;
    EPropertySpeed &ensure_property(MutableExtrusionEntity entity);
    FieldSummary merge_fields(const std::vector<FieldSummary> &fields) const;
    void clear_redundant_field(MutableExtrusionEntity entity, float inherited_value);
    FieldSummary hoist_tree(MutableExtrusionEntity entity,
                            const EffectiveTreeState &parent_state);

    ProcessField m_field;
    std::set<extrusion_entity_handle *> m_created_properties;
    std::set<extrusion_entity_handle *> m_modified_entities;
};

}}} // namespace slic3r_api::LayerExtrusionEdit::ProcessParameterHelpers

#endif // slic3r_Plugins_LayerExtrusionEdit_ExtrusionProcessParameterHelpers_hpp_
