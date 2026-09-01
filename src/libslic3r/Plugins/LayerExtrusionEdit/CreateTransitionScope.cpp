///|/ Copyright (c) SuperSlicer 2026 Durand Remi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/

#include "CreateTransitionScope.hpp"

#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

#include "ExtrusionScopeHelpers.hpp"
#include "libslic3r/Api/plugin/c/steps/slic3r_step_layer_extrusion_edit.h"
#include "libslic3r/Api/plugin/cpp/PluginBase.hpp"
#include "libslic3r/Api/plugin/cpp/PrintingPlanViews.hpp"
#include "libslic3r/Api/plugin/cpp/properties/ExtrusionProperties.hpp"

/*
Parallel compact transition-scope preparation
=============================================

setup() allocates one cache slot per final PrintingLayerGroup. setup_run()
fills those slots independently by reading immutable extrusion trees. After the
host barrier, run() may inspect neighbouring scalar descriptions but wraps only
scope roots owned by its layer.

The scan emits a small execution stream containing tool selections, process
operations and maximal continuous printable candidates. It never stores
borrowed property pointers. This makes cross-layer boundary decisions possible
without reading a neighbouring tree while that layer is being restructured.

Execution sequence
------------------

1. setup_impl() maps every {PrintingGroup, PrintingLayerGroup} pair to one flat
   cache index. It performs no extrusion-tree work.
2. setup_run_impl() runs independently for each layer. It converts that layer
   into an immutable stream of Scope, Process and ToolSelection entries.
3. The layer-extrusion-edit step places a barrier between all setup_run() calls
   and all run() calls. The complete stream is therefore read-only afterwards.
4. run_impl() finds the previous and next Scope entries in the global stream,
   replays the intervening scalar entries and derives compact boundary flags.
5. ensure_scope() finally mutates only roots belonging to the current layer.

Why use a stream instead of neighbouring trees?
-----------------------------------------------

Layer runs may execute in parallel. Reading another layer's extrusion tree
while its worker creates wrappers would be unsafe. The stream stores only
stable entity handles for local mutation and copied endpoint/process facts for
cross-layer decisions. No pointer to an extrusion property is retained.
*/

namespace slic3r_api { namespace LayerExtrusionEdit { namespace CreateTransitionScopePlugin {
namespace {

/* One absolute XYZ position in scaled PrintingPlan coordinates. */
struct PlannedPosition
{
    coord_t x = 0;
    coord_t y = 0;
    coord_t z = 0;
};

/*
Facts observed at one end of printable geometry.

Retraction modifiers are copied here because they can force a semantic
boundary even when adjacent positions are geometrically continuous.
*/
struct LeafBoundary
{
    PlannedPosition position;
    bool enforce_retraction = false;
    bool toolchange_retraction = false;
};

/*
Immutable description of one maximal printable scope candidate.

entity is retained only so the owning layer can wrap that exact stable root in
run_impl(). All data needed to compare it with another layer is copied by
value.
*/
struct ScopeDescriptor
{
    extrusion_entity_handle *entity = nullptr;
    LeafBoundary first;
    LeafBoundary last;
    uint16_t extruder_id = UINT16_MAX;
    // Travel providers publish their final result in these two bits. Preserve
    // them when this producer validates an already prepared scope.
    uint8_t preserved_materialized_flags = 0;
};

/* The three kinds of event required to replay final PrintingPlan order. */
enum class StreamEntryKind
{
    Scope,
    Process,
    ToolSelection
};

/*
One item in a layer's immutable execution stream.

Only the field selected by kind is meaningful. A simple struct is preferred to
a variant here because entries are internal, short-lived and validated at
every read site.
*/
struct StreamEntry
{
    StreamEntryKind kind = StreamEntryKind::Process;
    ScopeDescriptor scope;
    raw_extrusion_role process_role = RAW_EXTRUSION_ROLE_NONE;
    uint16_t selected_extruder = UINT16_MAX;
};

/* Cache slot written by one setup_run worker and read after the barrier. */
struct LayerScan
{
    std::vector<StreamEntry> entries;
};

/* Direct properties resolved over the inherited extrusion-tree state. */
struct EffectiveState
{
    coord_t z_offset = 0;
    std::optional<EPropertyAttributes> attributes;
    std::optional<EPropertyModifier> modifier;
};

/*
Bottom-up summary of one extrusion subtree.

The summary determines whether the subtree itself may become one scope. A
candidate needs printable geometry, no process geometry and no boundary
between its first and last printable descendants.
*/
struct EntityAnalysis
{
    bool has_printable = false;
    bool has_process = false;
    bool has_internal_boundary = false;
    LeafBoundary first;
    LeafBoundary last;
};

/* Stable index of one Scope entry in the flattened layer stream. */
struct ScopeLocation
{
    size_t layer_idx = size_t(-1);
    size_t entry_idx = size_t(-1);

    /* Return whether both indexes identify a real stream position. */
    bool valid() const
    {
        return layer_idx != size_t(-1) && entry_idx != size_t(-1);
    }
};

/* Boundary result reconstructed between two adjacent printable scopes. */
struct TransitionFacts
{
    bool boundary = false;
    bool materialized_travel = false;
};

/* Empty null-terminated dependency list required by PluginBase. */
const char *k_no_dependencies[] = { nullptr };

/* Add two scaled coordinates and report overflow instead of wrapping Z. */
coord_t checked_add(coord_t lhs, coord_t rhs);

/* Return true for geometry or events belonging to transition processing. */
bool is_process_role(raw_extrusion_role role);

/* Resolve direct attributes, modifiers and Z over the inherited parent state. */
EffectiveState resolved_state(const ExtrusionEntity &entity,
                              const EffectiveState &parent);

/* Convert one geometric leaf into absolute endpoint and modifier facts. */
EntityAnalysis analyze_local_geometry(const ExtrusionEntity &entity,
                                      coord_t print_z,
                                      const EffectiveState &state);

/* Return whether adjacent printable endpoints form a local transition. */
bool needs_geometric_or_semantic_boundary(const LeafBoundary &source,
                                          const LeafBoundary &target);

/* Analyze a subtree without producing candidates, used for existing scopes. */
EntityAnalysis analyze_entity(const ExtrusionEntity &entity,
                              coord_t print_z,
                              const EffectiveState &parent);

/*
Append maximal non-nested candidates and process tokens in execution order.

When a parent qualifies, child candidates emitted during recursion are replaced
by that parent. This keeps the highest usable existing root without a side map.
*/
EntityAnalysis append_entity_stream(
    MutableExtrusionEntity entity,
    coord_t print_z,
    const EffectiveState &parent,
    uint16_t extruder_id,
    const PluginPropertyKey<PrintingExtrusionScopeProperty> &scope_key,
    std::vector<StreamEntry> &output);

/* Scan one complete layer and preserve empty tool selections in the stream. */
std::vector<StreamEntry> collect_layer_stream(
    const PrintingLayerGroup &layer,
    const PluginPropertyKey<PrintingExtrusionScopeProperty> &scope_key);

/* Find the nearest scope before one stream location across layer boundaries. */
ScopeLocation previous_scope(const std::vector<LayerScan> &layers,
                             ScopeLocation current);

/* Find the nearest scope after one stream location across layer boundaries. */
ScopeLocation next_scope(const std::vector<LayerScan> &layers,
                         ScopeLocation current);

/* Read one scope descriptor from a validated stream location. */
const ScopeDescriptor &scope_at(const std::vector<LayerScan> &layers,
                                ScopeLocation location);

/*
Inspect only scalar stream entries between two adjacent printable scopes.

Tool selections are replayed so a transient empty tool-group still creates a
boundary even when source and target use the same extruder.
*/
TransitionFacts transition_between(const std::vector<LayerScan> &layers,
                                   ScopeLocation source,
                                   ScopeLocation target);

/*
Built-in producer of compact transition scopes.

The instance owns the dynamic property key and per-execution scan cache. The
host calls setup_impl(), every setup_run_impl(), then every run_impl(). See the
file introduction for the synchronization contract between those methods.
*/
class CreateTransitionScope : public PluginBase
{
public:
    /* Return the process-wide built-in instance registered in one orchestrator. */
    static CreateTransitionScope &instance(orchestrator_handle *orchestrator);

    /* Register the private scope payload and initialize the plugin facade. */
    explicit CreateTransitionScope(orchestrator_handle *orchestrator);

private:
    /* Plugin identity and user-facing metadata consumed by the orchestrator. */
    const char *id_impl() const noexcept override;
    const char *name_impl() const noexcept override;
    const char *description_impl() const noexcept override;
    const char *exclusive_group_impl() const noexcept override;
    const char *exclusive_group_label_impl() const noexcept override;
    const char *exclusive_group_tooltip_impl() const noexcept override;
    slicing_step_t step_impl() const noexcept override;
    const char *const *dependencies_impl() const noexcept override;
    int32_t priority_impl() const noexcept override;
    const char *progress_message_format_impl() const noexcept override;

    /* Allocate the flat cache without traversing extrusion geometry. */
    void setup_impl(const plugin_run_context *run_ctx, uint32_t run_count) const override;

    /* Scan one immutable layer into the worker's exclusive cache slot. */
    void setup_run_impl(const plugin_run_context *run_ctx) const override;

    /* Derive neighbour flags and wrap only roots owned by the current layer. */
    void run_impl(const plugin_run_context *run_ctx) const override;

    /* Convert a stable group/layer location into the pre-sized cache index. */
    size_t flat_layer_index(uint32_t group_idx, uint32_t layer_idx) const;

    /* Runtime property identity shared with downstream transition plugins. */
    PluginPropertyKey<PrintingExtrusionScopeProperty> m_scope_property;

    /* False until setup_impl() has published a complete cache topology. */
    mutable bool m_setup_valid = false;

    /* Maps final PrintingPlan group/layer indexes to m_layers. */
    mutable std::vector<std::vector<size_t>> m_flat_indices;

    /* One independently written scan slot per final PrintingLayerGroup. */
    mutable std::vector<LayerScan> m_layers;
};

/* Add scaled coordinates without permitting signed overflow. */
coord_t checked_add(const coord_t lhs, const coord_t rhs)
{
    // Z combines layer height, inherited offsets and segment offsets. Validate
    // both additions so malformed plugin geometry cannot wrap scaled space.
    if ((rhs > 0 && lhs > (std::numeric_limits<coord_t>::max)() - rhs) ||
        (rhs < 0 && lhs < (std::numeric_limits<coord_t>::min)() - rhs))
        throw std::overflow_error("A transition-scope endpoint exceeds coord_t.");
    return lhs + rhs;
}

/* Classify geometry that participates in a transition instead of print content. */
bool is_process_role(const raw_extrusion_role role)
{
    return RAW_EXTRUSION_ROLE_IS_TRAVEL(role) ||
        RAW_EXTRUSION_ROLE_IS_WIPE(role) ||
        RAW_EXTRUSION_ROLE_IS_RETRACT(role) ||
        RAW_EXTRUSION_ROLE_IS_UNRETRACT(role);
}

/* Resolve the three properties needed by the scanner at one tree node. */
EffectiveState resolved_state(const ExtrusionEntity &entity,
                              const EffectiveState &parent)
{
    // Start with inherited values, then replace only properties stored directly
    // on this entity. Descendants receive the resulting state by value.
    EffectiveState state = parent;
    if (const EPropertyZOffset *offset = entity.get(EPropertyZOffset::key))
        state.z_offset = offset->get();
    if (const EPropertyAttributes *attributes = entity.get(EPropertyAttributes::key))
        state.attributes = *attributes;
    if (const EPropertyModifier *modifier = entity.get(EPropertyModifier::key))
        state.modifier = *modifier;
    return state;
}

/* Summarize one local geometric leaf in absolute PrintingPlan coordinates. */
EntityAnalysis analyze_local_geometry(const ExtrusionEntity &entity,
                                      const coord_t print_z,
                                      const EffectiveState &state)
{
    EntityAnalysis analysis;

    // Process geometry is emitted into the execution stream but can never be
    // selected as printable scope content.
    const raw_extrusion_role role = state.attributes ?
        raw_extrusion_role(state.attributes->extrusion_role()) :
        RAW_EXTRUSION_ROLE_NONE;
    if (is_process_role(role)) {
        analysis.has_process = true;
        return analysis;
    }
    if (entity.segment_count() == 0)
        return analysis;

    // A scope comparison needs only its first and last points. Interior gaps
    // are discovered separately while aggregating child analyses.
    const c_extrusion_segment first_segment = entity.segment(0);
    const c_extrusion_segment last_segment =
        entity.segment(entity.segment_count() - 1);
    analysis.has_printable = true;
    analysis.first.position = PlannedPosition{
        first_segment.point_a.x,
        first_segment.point_a.y,
        checked_add(checked_add(print_z, state.z_offset), first_segment.z_offset_a)};
    analysis.last.position = PlannedPosition{
        last_segment.point_b.x,
        last_segment.point_b.y,
        checked_add(checked_add(print_z, state.z_offset), last_segment.z_offset_b)};

    // Modifiers are repeated on both ends because either side of a pair may
    // force a semantic transition when two candidates are compared.
    if (state.modifier) {
        analysis.first.enforce_retraction =
            state.modifier->enforce_retraction != 0;
        analysis.first.toolchange_retraction =
            state.modifier->toolchange_retraction != 0;
        analysis.last.enforce_retraction = analysis.first.enforce_retraction;
        analysis.last.toolchange_retraction =
            analysis.first.toolchange_retraction;
    }
    return analysis;
}

/* Decide whether two printable endpoints require process phases between them. */
bool needs_geometric_or_semantic_boundary(const LeafBoundary &source,
                                          const LeafBoundary &target)
{
    // Explicit retraction requests take precedence over geometric continuity.
    if (source.enforce_retraction || target.enforce_retraction ||
        source.toolchange_retraction || target.toolchange_retraction)
        return true;

    // Compare in XYZ, not only XY: a pure Z discontinuity still needs a travel.
    // Long double avoids overflowing coord_t while squaring large deltas.
    const long double dx =
        static_cast<long double>(target.position.x) - source.position.x;
    const long double dy =
        static_cast<long double>(target.position.y) - source.position.y;
    const long double dz =
        static_cast<long double>(target.position.z) - source.position.z;
    return std::sqrt(dx * dx + dy * dy + dz * dz) >=
        static_cast<long double>(SCALED_EPSILON);
}

/* Analyze a subtree bottom-up without retaining or mutating descendant views. */
EntityAnalysis analyze_entity(const ExtrusionEntity &entity,
                              const coord_t print_z,
                              const EffectiveState &parent)
{
    const EffectiveState state = resolved_state(entity, parent);

    // A geometric entity, or an empty event leaf, has no child sequence to
    // aggregate and is handled by the local analyzer.
    if (entity.segment_count() != 0 || entity.child_count() == 0)
        return analyze_local_geometry(entity, print_z, state);

    EntityAnalysis result;

    // Merge children in execution order. A gap between consecutive printable
    // children prevents their parent from becoming one maximal scope.
    for (uint32_t child_idx = 0; child_idx < entity.child_count(); ++child_idx) {
        const EntityAnalysis child = analyze_entity(
            entity.child(child_idx), print_z, state);
        result.has_process = result.has_process || child.has_process;
        result.has_internal_boundary =
            result.has_internal_boundary || child.has_internal_boundary;
        if (!child.has_printable)
            continue;
        if (result.has_printable &&
            needs_geometric_or_semantic_boundary(result.last, child.first))
            result.has_internal_boundary = true;
        if (!result.has_printable)
            result.first = child.first;
        result.last = child.last;
        result.has_printable = true;
    }
    return result;
}

/*
Emit stream entries for one subtree and return the same bottom-up analysis.

Recursion first emits the finest usable candidates. If the current parent is a
better maximal scope, output is rolled back to output_begin and replaced by
that parent. This yields non-nested roots without an auxiliary ownership map.
*/
EntityAnalysis append_entity_stream(
    MutableExtrusionEntity entity,
    const coord_t print_z,
    const EffectiveState &parent,
    const uint16_t extruder_id,
    const PluginPropertyKey<PrintingExtrusionScopeProperty> &scope_key,
    std::vector<StreamEntry> &output)
{
    const EffectiveState state = resolved_state(entity.readonly(), parent);

    // A marker from an earlier execution is already a scope boundary. Validate
    // its content and treat the stable marked root as one indivisible candidate.
    if (ExtrusionScope::is_scope(entity.readonly(), scope_key)) {
        const ExtrusionScope::OrderedExtrusionScope scope(entity, scope_key);
        const PrintingExtrusionScopeProperty *existing = entity.get(scope_key);
        if (existing == nullptr)
            throw std::runtime_error("An existing extrusion scope lost its marker.");
        const MutableExtrusionEntity content = scope.content();
        const EntityAnalysis analysis = analyze_entity(
            content.readonly(), print_z, state);
        if (!analysis.has_printable || analysis.has_process ||
            analysis.has_internal_boundary || !content.continuous())
            throw std::runtime_error(
                "An existing transition scope no longer contains one continuous printable scope.");
        StreamEntry entry;
        entry.kind = StreamEntryKind::Scope;
        const uint8_t materialized_flags = uint8_t(existing->flags &
            (PRINTING_EXTRUSION_SCOPE_INCOMING_TRAVEL_MATERIALIZED |
             PRINTING_EXTRUSION_SCOPE_OUTGOING_TRAVEL_MATERIALIZED));
        entry.scope = ScopeDescriptor{
            entity.mutable_handle(), analysis.first, analysis.last,
            extruder_id, materialized_flags};
        output.push_back(entry);
        return analysis;
    }

    // Leaves become either a process token, a printable scope candidate or no
    // stream entry at all when they carry no geometry relevant to this stage.
    if (entity.segment_count() != 0 || entity.child_count() == 0) {
        const EntityAnalysis analysis = analyze_local_geometry(
            entity.readonly(), print_z, state);
        const raw_extrusion_role role = state.attributes ?
            raw_extrusion_role(state.attributes->extrusion_role()) :
            RAW_EXTRUSION_ROLE_NONE;
        if (analysis.has_process) {
            StreamEntry entry;
            entry.kind = StreamEntryKind::Process;
            entry.process_role = role;
            output.push_back(entry);
        } else if (analysis.has_printable) {
            StreamEntry entry;
            entry.kind = StreamEntryKind::Scope;
            entry.scope = ScopeDescriptor{
                entity.mutable_handle(), analysis.first, analysis.last, extruder_id};
            output.push_back(entry);
        }
        return analysis;
    }

    // Remember where descendants begin in the stream. A qualifying parent can
    // replace exactly this suffix after all children have been analyzed.
    const size_t output_begin = output.size();
    EntityAnalysis result;

    // Preserve prefix order while aggregating process facts and detecting gaps
    // between consecutive printable descendants.
    for (uint32_t child_idx = 0; child_idx < entity.child_count(); ++child_idx) {
        const EntityAnalysis child = append_entity_stream(
            entity.child_mutable(child_idx), print_z, state, extruder_id,
            scope_key, output);
        result.has_process = result.has_process || child.has_process;
        result.has_internal_boundary =
            result.has_internal_boundary || child.has_internal_boundary;
        if (!child.has_printable)
            continue;
        if (result.has_printable &&
            needs_geometric_or_semantic_boundary(result.last, child.first))
            result.has_internal_boundary = true;
        if (!result.has_printable)
            result.first = child.first;
        result.last = child.last;
        result.has_printable = true;
    }

    // Sortable parents are never scopes because another stage may reorder them.
    // A continuous fixed parent with no process or internal boundary is the
    // highest existing root that safely represents the whole printable run.
    const bool can_be_one_scope = entity.continuous() &&
        !entity.readonly().sortable() &&
        result.has_printable && !result.has_process &&
        !result.has_internal_boundary;
    if (can_be_one_scope) {
        output.resize(output_begin);
        StreamEntry entry;
        entry.kind = StreamEntryKind::Scope;
        entry.scope = ScopeDescriptor{
            entity.mutable_handle(), result.first, result.last, extruder_id};
        output.push_back(entry);
    }
    return result;
}

/* Convert one PrintingLayerGroup into its immutable execution-order stream. */
std::vector<StreamEntry> collect_layer_stream(
    const PrintingLayerGroup &layer,
    const PluginPropertyKey<PrintingExtrusionScopeProperty> &scope_key)
{
    std::vector<StreamEntry> output;
    const EffectiveState empty_state;

    // Emit every tool selection, including empty tool-groups. A temporary tool
    // change may be semantically important even when no extrusion follows it.
    for (uint32_t tool_idx = 0; tool_idx < layer.tool_group_count(); ++tool_idx) {
        const PrintingToolGroup tool = layer.tool_group(tool_idx);
        StreamEntry selection;
        selection.kind = StreamEntryKind::ToolSelection;
        selection.selected_extruder = tool.extruder_id();
        output.push_back(selection);

        // PrintingExtrusion roots remain separate ownership domains. Their
        // candidates may be geometrically contiguous but are never coalesced.
        for (uint32_t extrusion_idx = 0;
             extrusion_idx < tool.extrusion_count(); ++extrusion_idx) {
            const PrintingExtrusion extrusion = tool.extrusion(extrusion_idx);
            append_entity_stream(extrusion.mutable_root(), layer.print_z(),
                                 empty_state, tool.extruder_id(), scope_key,
                                 output);
        }
    }
    return output;
}

/* Walk backward through immutable streams until the preceding Scope entry. */
ScopeLocation previous_scope(const std::vector<LayerScan> &layers,
                             ScopeLocation current)
{
    size_t layer_idx = current.layer_idx;
    size_t entry_idx = current.entry_idx;
    while (true) {
        while (entry_idx > 0) {
            --entry_idx;
            if (layers[layer_idx].entries[entry_idx].kind == StreamEntryKind::Scope)
                return ScopeLocation{layer_idx, entry_idx};
        }
        if (layer_idx == 0)
            return ScopeLocation();
        --layer_idx;
        entry_idx = layers[layer_idx].entries.size();
    }
}

/* Walk forward through immutable streams until the following Scope entry. */
ScopeLocation next_scope(const std::vector<LayerScan> &layers,
                         ScopeLocation current)
{
    size_t layer_idx = current.layer_idx;
    size_t entry_idx = current.entry_idx + 1;
    while (layer_idx < layers.size()) {
        while (entry_idx < layers[layer_idx].entries.size()) {
            if (layers[layer_idx].entries[entry_idx].kind == StreamEntryKind::Scope)
                return ScopeLocation{layer_idx, entry_idx};
            ++entry_idx;
        }
        ++layer_idx;
        entry_idx = 0;
    }
    return ScopeLocation();
}

/* Validate and dereference a Scope location produced by neighbour lookup. */
const ScopeDescriptor &scope_at(const std::vector<LayerScan> &layers,
                                const ScopeLocation location)
{
    if (!location.valid() || location.layer_idx >= layers.size() ||
        location.entry_idx >= layers[location.layer_idx].entries.size() ||
        layers[location.layer_idx].entries[location.entry_idx].kind !=
            StreamEntryKind::Scope)
        throw std::runtime_error("A transition boundary references an invalid scope.");
    return layers[location.layer_idx].entries[location.entry_idx].scope;
}

/*
Reconstruct transition facts between two adjacent printable scope entries.

The function replays only copied scalar tokens. It never opens either scope's
tree, which is what allows different layer runs to mutate their own roots in
parallel after setup_run's barrier.
*/
TransitionFacts transition_between(const std::vector<LayerScan> &layers,
                                   const ScopeLocation source,
                                   const ScopeLocation target)
{
    const ScopeDescriptor &source_scope = scope_at(layers, source);
    const ScopeDescriptor &target_scope = scope_at(layers, target);
    TransitionFacts facts;
    uint16_t selected_extruder = source_scope.extruder_id;

    // Replay all tool and process entries strictly between source and target.
    // A pure Travel marks an already materialized connection; Wipe remains a
    // process boundary but is not the target scope's reserved travel.
    for (size_t layer_idx = source.layer_idx; layer_idx <= target.layer_idx; ++layer_idx) {
        const std::vector<StreamEntry> &entries = layers[layer_idx].entries;
        const size_t begin = layer_idx == source.layer_idx ? source.entry_idx + 1 : 0;
        const size_t end = layer_idx == target.layer_idx ? target.entry_idx : entries.size();
        for (size_t entry_idx = begin; entry_idx < end; ++entry_idx) {
            const StreamEntry &entry = entries[entry_idx];
            if (entry.kind == StreamEntryKind::ToolSelection) {
                if (entry.selected_extruder != selected_extruder)
                    facts.boundary = true;
                selected_extruder = entry.selected_extruder;
            } else if (entry.kind == StreamEntryKind::Process) {
                facts.boundary = true;
                if (RAW_EXTRUSION_ROLE_IS_TRAVEL(entry.process_role) &&
                    !RAW_EXTRUSION_ROLE_IS_WIPE(entry.process_role))
                    facts.materialized_travel = true;
            }
        }
    }

    // The target owner and endpoint facts complete the transition decision.
    // This also catches a direct tool change with no intervening selection token.
    if (selected_extruder != target_scope.extruder_id)
        facts.boundary = true;
    if (needs_geometric_or_semantic_boundary(
            source_scope.last, target_scope.first))
        facts.boundary = true;
    return facts;
}

/* Return the singleton used for registration and every execution callback. */
CreateTransitionScope &CreateTransitionScope::instance(
    orchestrator_handle *orchestrator)
{
    // Built-in plugins are process-wide singletons. The first registration
    // supplies the orchestrator whose dynamic property registry they use.
    static CreateTransitionScope plugin(orchestrator);
    return plugin;
}

/* Initialize PluginBase and register the shared private scope payload. */
CreateTransitionScope::CreateTransitionScope(orchestrator_handle *orchestrator) :
    PluginBase(orchestrator),
    m_scope_property(printing_extrusion_scope_property_key(orchestrator))
{
}

/* Return the stable provider identifier used by activation configuration. */
const char *CreateTransitionScope::id_impl() const noexcept
{
    return "layer_extrusion_edit.transition_scope.default";
}

/* Return the short provider name shown by plugin-selection interfaces. */
const char *CreateTransitionScope::name_impl() const noexcept
{
    return "Default compact transition scopes";
}

/* Summarize the provider's observable responsibility. */
const char *CreateTransitionScope::description_impl() const noexcept
{
    return "Marks continuous printable scopes and reserves phases at real transitions.";
}

/* Place alternative transition-scope producers in one exclusive group. */
const char *CreateTransitionScope::exclusive_group_impl() const noexcept
{
    return "layer_extrusion_edit.transition_scope";
}

/* Return the user-facing name of the exclusive provider group. */
const char *CreateTransitionScope::exclusive_group_label_impl() const noexcept
{
    return "Extrusion transition scopes";
}

/* Explain what selecting another provider in this group changes. */
const char *CreateTransitionScope::exclusive_group_tooltip_impl() const noexcept
{
    return "Selects how ordered extrusion boundaries are prepared for process plugins.";
}

/* Run once per final PrintingLayerGroup in the parallel extrusion-edit step. */
slicing_step_t CreateTransitionScope::step_impl() const noexcept
{
    return STEP_LAYER_EXTRUSION_EDIT;
}

/* Declare no provider dependency: this is the first transition pipeline stage. */
const char *const *CreateTransitionScope::dependencies_impl() const noexcept
{
    return k_no_dependencies;
}

/* Execute before retraction, wipe, entry state, travel and lift providers. */
int32_t CreateTransitionScope::priority_impl() const noexcept
{
    return -100;
}

/* Return the progress text used while layer workers publish their scans. */
const char *CreateTransitionScope::progress_message_format_impl() const noexcept
{
    return "Preparing compact extrusion scopes: %u / %u layers";
}

/* Translate structural indexes from a run context into the flat scan cache. */
size_t CreateTransitionScope::flat_layer_index(const uint32_t group_idx,
                                               const uint32_t layer_idx) const
{
    if (group_idx >= m_flat_indices.size() ||
        layer_idx >= m_flat_indices[group_idx].size())
        return size_t(-1);
    return m_flat_indices[group_idx][layer_idx];
}

/*
Prepare cache topology for one complete execution.

This deliberately performs only cheap structural counting. Geometry remains in
setup_run_impl(), where each PrintingLayerGroup can be scanned in parallel.
*/
void CreateTransitionScope::setup_impl(const plugin_run_context *run_ctx,
                                       const uint32_t run_count) const
{
    // Invalidate previous topology before inspecting the new context so an
    // early return cannot expose stale scan entries to later callbacks.
    m_setup_valid = false;
    m_flat_indices.clear();
    m_layers.clear();
    const run_ctx_layer_extrusion_edition *ctx =
        plugin_ctx_as_layer_extrusion_edition(run_ctx);
    if (ctx == nullptr || ctx->plan == nullptr)
        return;

    // Build a deterministic flat order matching PrintingPlan group/layer order.
    // This order is later used to find neighbours across layer and group edges.
    const PrintingPlan plan(ctx->plan);
    m_flat_indices.resize(plan.group_count());
    size_t flat_idx = 0;
    for (uint32_t group_idx = 0; group_idx < plan.group_count(); ++group_idx) {
        const PrintingGroup group = plan.group(group_idx);
        m_flat_indices[group_idx].resize(group.layer_group_count());
        for (uint32_t layer_idx = 0;
             layer_idx < group.layer_group_count(); ++layer_idx)
            m_flat_indices[group_idx][layer_idx] = flat_idx++;
    }
    if (flat_idx != run_count)
        throw std::runtime_error(
            "The transition-scope run count does not match the PrintingPlan.");
    m_layers.resize(flat_idx);

    // Publish validity only after every mapping and cache slot exists.
    m_setup_valid = true;
}

/* Scan one layer into the cache element exclusively owned by this worker. */
void CreateTransitionScope::setup_run_impl(
    const plugin_run_context *run_ctx) const
{
    if (!m_setup_valid)
        return;
    const run_ctx_layer_extrusion_edition *ctx =
        plugin_ctx_as_layer_extrusion_edition(run_ctx);
    if (ctx == nullptr || ctx->layer_group == nullptr)
        return;
    const size_t flat_idx = flat_layer_index(
        ctx->group_idx, ctx->layer_group_idx);
    if (flat_idx >= m_layers.size())
        throw std::runtime_error(
            "A transition-scope run references an unknown layer.");

    // Every worker owns one pre-sized element. The host barrier completes all
    // writes before any run reads neighbouring descriptions.
    m_layers[flat_idx].entries = collect_layer_stream(
        PrintingLayerGroup(ctx->layer_group), m_scope_property);
    progress().add_max(1);
}

/*
Apply compact scope layouts to roots owned by one layer.

All setup-run workers have completed before this method starts. It may inspect
neighbouring immutable stream entries, but it never mutates the entity handles
stored in another layer's cache slot.
*/
void CreateTransitionScope::run_impl(const plugin_run_context *run_ctx) const
{
    if (!m_setup_valid)
        return;
    const run_ctx_layer_extrusion_edition *ctx =
        plugin_ctx_as_layer_extrusion_edition(run_ctx);
    if (ctx == nullptr || ctx->layer_group == nullptr)
        return;
    const size_t flat_idx = flat_layer_index(
        ctx->group_idx, ctx->layer_group_idx);
    if (flat_idx >= m_layers.size())
        throw std::runtime_error(
            "A transition-scope run references an unknown layer.");

    const std::vector<StreamEntry> &entries = m_layers[flat_idx].entries;

    // Process only Scope tokens. Tool selections and process operations exist
    // solely to explain the boundaries between successive Scope tokens.
    for (size_t entry_idx = 0; entry_idx < entries.size(); ++entry_idx) {
        if (entries[entry_idx].kind != StreamEntryKind::Scope)
            continue;
        const ScopeLocation current{flat_idx, entry_idx};
        const ScopeLocation previous = previous_scope(m_layers, current);
        const ScopeLocation next = next_scope(m_layers, current);
        uint8_t flags = 0;

        // The first global scope has no incoming phases. Otherwise derive its
        // incoming flags from exactly the same source/target pair used by the
        // previous scope's outgoing side.
        if (!previous.valid()) {
            flags = uint8_t(flags | PRINTING_EXTRUSION_SCOPE_START);
        } else {
            const TransitionFacts incoming = transition_between(
                m_layers, previous, current);
            if (incoming.boundary)
                flags = uint8_t(flags |
                    PRINTING_EXTRUSION_SCOPE_INCOMING_TRANSITION);
            if (incoming.boundary && incoming.materialized_travel)
                flags = uint8_t(flags |
                    PRINTING_EXTRUSION_SCOPE_INCOMING_TRAVEL_MATERIALIZED);
        }

        // A terminal scope receives no artificial outgoing transition. A real
        // neighbour is the only reason to reserve an after phase.
        if (!next.valid()) {
            flags = uint8_t(flags | PRINTING_EXTRUSION_SCOPE_TERMINAL);
        } else {
            const TransitionFacts outgoing = transition_between(
                m_layers, current, next);
            if (outgoing.boundary)
                flags = uint8_t(flags |
                    PRINTING_EXTRUSION_SCOPE_OUTGOING_TRANSITION);
            if (outgoing.boundary && outgoing.materialized_travel)
                flags = uint8_t(flags |
                    PRINTING_EXTRUSION_SCOPE_OUTGOING_TRAVEL_MATERIALIZED);
        }

        // Only the current layer owns this descriptor's root. Construction is
        // therefore safe while other run workers wrap their respective layers.
        const ScopeDescriptor &descriptor = entries[entry_idx].scope;
        if (descriptor.entity == nullptr)
            throw std::runtime_error(
                "A transition scope descriptor has no entity handle.");
        // Initial stream facts define existing process travels. Bits published
        // by a travel provider are final runtime state and remain authoritative
        // when this producer validates an already prepared plan again.
        if ((flags & PRINTING_EXTRUSION_SCOPE_INCOMING_TRANSITION) != 0)
            flags = uint8_t(flags | (descriptor.preserved_materialized_flags &
                PRINTING_EXTRUSION_SCOPE_INCOMING_TRAVEL_MATERIALIZED));
        if ((flags & PRINTING_EXTRUSION_SCOPE_OUTGOING_TRANSITION) != 0)
            flags = uint8_t(flags | (descriptor.preserved_materialized_flags &
                PRINTING_EXTRUSION_SCOPE_OUTGOING_TRAVEL_MATERIALIZED));
        ExtrusionScope::ensure_scope(
            MutableExtrusionEntity(descriptor.entity), m_scope_property, flags);
    }
    progress().increment();
}

} // namespace

/* Publish the built-in singleton through the orchestrator's plugin registry. */
void register_create_transition_scope_plugin(orchestrator_handle *orchestrator)
{
    orchestrator_register_plugin(
        orchestrator, CreateTransitionScope::instance(orchestrator).c_instance());
}

}}} // namespace slic3r_api::LayerExtrusionEdit::CreateTransitionScopePlugin
