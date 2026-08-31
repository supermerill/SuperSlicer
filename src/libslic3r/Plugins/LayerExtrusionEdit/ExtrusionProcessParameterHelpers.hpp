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
uses EPropertySpeedFieldEditor to write and hoist only its assigned field.
*/

namespace slic3r_api { namespace LayerExtrusionEdit { namespace ProcessParameterHelpers {

enum class LeafDisposition
{
    Empty,
    Editable,
    Excluded
};

enum class EPropertySpeedField
{
    // Linear movement speed stored in EPropertySpeed::speed_mm_per_s.
    Speed,
    // Movement acceleration stored in EPropertySpeed::accel_mm_per_s2.
    Acceleration,
    // Requested fan percentage stored in EPropertySpeed::fan_speed_percent.
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
                                 EPropertySpeedField field);

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

/*
Edit one field of EPropertySpeed without making the caller manage the shared
property payload or the extrusion tree's inheritance rules.

EPropertySpeed stores several independent process parameters. A speed plugin,
for example, must be able to set speed without resetting acceleration, fan,
pressure advance, or temperature values written by another plugin. This editor
therefore reuses an existing property when possible and fully initializes a new
property with the unset value (-1) before writing only the field selected by
the constructor.

After assigning leaf values, call hoist() once on the root. It moves an exactly
uniform effective value to the nearest common parent and removes only redundant
direct values for the selected field. Hoisting stops at excluded leaves,
unresolved values, or conflicting values, so it never gives a process value to
a subtree for which that value was not proven valid. Fan values receive an
additional ownership check: a pre-existing direct fan override is not removed
merely because it equals a value hoisted by this pass.

The editor is intended for one field and one mutation pass. It keeps borrowed
entity handles so it can remove payloads that it created when they become empty,
report the remaining generated annotations, and protect pre-existing fan
overrides. It must not outlive structural mutations that invalidate handles.

Typical use:

    EPropertySpeedFieldEditor editor(EPropertySpeedField::Acceleration);
    editor.set_value(leaf, acceleration);
    editor.hoist(root);
*/
class EPropertySpeedFieldEditor
{
public:
    // Select the sole EPropertySpeed field that this editor may change.
    explicit EPropertySpeedFieldEditor(EPropertySpeedField field);

    // Tracking sets are local to one mutation pass and must not be copied.
    EPropertySpeedFieldEditor(const EPropertySpeedFieldEditor &) = delete;
    EPropertySpeedFieldEditor &operator=(const EPropertySpeedFieldEditor &) = delete;

    /*
    Set the selected field directly on entity while preserving every other
    EPropertySpeed field. Speed and acceleration require a positive value; fan
    speed also accepts zero. An unset or invalid value is ignored.

    The entity is recorded as modified so hoist() can remove this direct value
    later if the same value is proven to be inherited from a parent.
    */
    void set_value(MutableExtrusionEntity entity, float value);

    /*
    Compact the selected field throughout root after leaf values have been
    assigned. A value is moved towards a parent only when every relevant
    descendant has exactly the same effective float value. Excluded, unresolved
    or conflicting subtrees prevent that move.

    Other EPropertySpeed fields and their containing payload are preserved. A
    redundant direct speed or acceleration may be cleared even if it predates
    this editor; a pre-existing fan override is retained. This operation does
    not reorder or otherwise reshape the tree.
    */
    void hoist(MutableExtrusionEntity root);

    /*
    Return the borrowed handles that still carry a direct selected-field value
    written by this editor after the latest hoist(). The set is useful to mark
    generated annotations for later cleanup; it does not transfer ownership.

    Structural mutation of the extrusion tree may invalidate these handles.
    */
    const std::set<extrusion_entity_handle *> &modified_entities() const {
        return m_modified_entities;
    }

private:
    // Summarize whether one subtree has a single hoistable effective value.
    struct FieldSummary
    {
        bool has_value = false;
        bool blocked = false;
        float value = -1.f;
    };

    // Read the selected field from the effective state inherited by a node.
    float state_value(const EffectiveTreeState &state) const;
    // Apply the selected field's validity rule, including valid zero fan speed.
    bool value_is_set(float value) const;
    // Read the selected direct field, or return the unset sentinel when absent.
    float direct_value(const EPropertySpeed *process) const;
    // Return a mutable reference to the selected member of an existing payload.
    float &field(EPropertySpeed &process) const;
    // Reuse or create a fully initialized EPropertySpeed on entity.
    EPropertySpeed &ensure_property(MutableExtrusionEntity entity);
    // Combine child summaries and mark disagreements or exclusions as blocked.
    FieldSummary merge_fields(const std::vector<FieldSummary> &fields) const;
    // Clear a direct value made redundant by an equal inherited value.
    void clear_redundant_field(MutableExtrusionEntity entity, float inherited_value);
    // Hoist children first and return the selected field's subtree summary.
    FieldSummary hoist_tree(MutableExtrusionEntity entity,
                            const EffectiveTreeState &parent_state);

    // Field selected for the lifetime of this editor.
    EPropertySpeedField m_field;
    // Properties allocated by this editor and removable when wholly empty.
    std::set<extrusion_entity_handle *> m_created_properties;
    // Entities that currently retain a direct value written by this editor.
    std::set<extrusion_entity_handle *> m_modified_entities;
};

}}} // namespace slic3r_api::LayerExtrusionEdit::ProcessParameterHelpers

#endif // slic3r_Plugins_LayerExtrusionEdit_ExtrusionProcessParameterHelpers_hpp_
