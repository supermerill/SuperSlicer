///|/ Copyright (c) SuperSlicer 2026 Durand Remi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/

/*
Semantic retraction planning for compact PrintingPlan scopes
============================================================

The transition producer has already decided where process boundaries exist.
This module resolves the retraction settings at those boundaries and appends
firmware-neutral E-axis requests to the existing ordered phases. Local scope
pairs use their owner views directly. Only layer-edge values are copied into a
small scalar cache so parallel workers never inspect or mutate another layer.
*/

#include "CreateRetraction.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
#include <stdexcept>
#include <vector>

#include "ExtrusionScopeHelpers.hpp"
#include "libslic3r/Api/plugin/c/steps/slic3r_step_extrusion_edit.h"
#include "libslic3r/Api/plugin/c/steps/slic3r_step_layer_extrusion_edit.h"
#include "libslic3r/Api/plugin/cpp/ConfigViews.hpp"
#include "libslic3r/Api/plugin/cpp/PluginBase.hpp"
#include "libslic3r/Api/plugin/cpp/PrintingEntityPropertyTraversal.hpp"
#include "libslic3r/Api/plugin/cpp/PrintingPlanViews.hpp"
#include "libslic3r/Api/plugin/cpp/RegionSettingsViews.hpp"
#include "libslic3r/Api/plugin/cpp/properties/ExtrusionProperties.hpp"

namespace slic3r_api { namespace LayerExtrusionEdit { namespace CreateRetractionPlugin {
namespace {

using ScopeEntity = PrintingEntity<PrintingExtrusionScopeProperty>;

const char *const k_scope_dependencies[] = {
    "layer_extrusion_edit.transition_scope.default",
    nullptr
};

const char *const k_no_dependencies[] = {nullptr};

const RegionSettings::OptionKeyGroup k_retraction_region_keys = {
    "print_retract_length"
};

/* One absolute endpoint in scaled PrintingPlan coordinates. */
struct PlannedPosition
{
    coord_t x = 0;
    coord_t y = 0;
    coord_t z = 0;
};

/* Properties inherited while descending from a PrintingExtrusion root. */
struct EffectiveState
{
    coord_t z_offset = 0;
    std::optional<EPropertyModifier> modifier;
};

/* Geometric endpoint and its tiny inside probe for regional lookup. */
struct GeometricEndpoint
{
    MutableExtrusionEntity leaf;
    PlannedPosition position;
    EPropertyModifier modifier{};
    c_point interior_point{};
    bool has_interior_point = false;
};

/* Scalar facts required to decide one side of a transition. */
struct BoundaryState
{
    bool valid = false;
    PlannedPosition position;
    EPropertyModifier modifier{};
    uint16_t extruder_id = UINT16_MAX;
    double normal_retract_target = 0.0;
};

/* Immutable values published by one setup_run worker. */
struct LayerBoundarySummary
{
    bool prepared = false;
    BoundaryState first;
    BoundaryState last;
    bool first_incoming_toolchange = false;
    bool last_outgoing_toolchange = false;
};

/* Complete process decision calculated identically from either layer side. */
struct RetractionDecision
{
    bool requested = false;
    bool toolchange_retraction = false;
    double target = 0.0;
    double restart_extra = 0.0;
};

/* Add scaled coordinates without allowing signed overflow. */
coord_t checked_add(coord_t lhs, coord_t rhs);

/* Resolve direct Z and modifier properties over inherited state. */
EffectiveState resolved_state(const ExtrusionEntity &entity,
                              const EffectiveState &parent);

/* Locate the parent state of a marked scope inside its owning extrusion tree. */
bool find_scope_parent_state(MutableExtrusionEntity entity,
                             const extrusion_entity_handle *scope_handle,
                             const EffectiveState &parent,
                             EffectiveState &scope_parent);

/* Resolve the first geometric leaf under one entity. */
bool find_first_geometry(MutableExtrusionEntity entity, coord_t print_z,
                         const EffectiveState &parent,
                         GeometricEndpoint &endpoint);

/* Resolve the last geometric leaf under one entity. */
bool find_last_geometry(MutableExtrusionEntity entity, coord_t print_z,
                        const EffectiveState &parent,
                        GeometricEndpoint &endpoint);

/* Resolve one scope endpoint while retaining only its effective leaf state. */
GeometricEndpoint scope_endpoint(const ScopeEntity &scope,
                                 const PluginPropertyKey<PrintingExtrusionScopeProperty> &key,
                                 bool first);

/* Read the source-region override for the normal retraction target. */
double normal_retract_target(const Config &print_config,
                             const ScopeEntity &source,
                             const GeometricEndpoint &endpoint);

/* Convert a scope side into the scalar decision format. */
BoundaryState boundary_state(const ScopeEntity &scope,
                             const Config &print_config,
                             const PluginPropertyKey<PrintingExtrusionScopeProperty> &key,
                             bool first,
                             bool resolve_normal_target);

/* Return the direct XY gap used by retract_before_travel. */
double planar_gap_mm(const BoundaryState &source,
                     const BoundaryState &target);

/* Validate matching scope flags and report a real tool selection. */
bool boundary_toolchange(const PrintingExtrusionScopeProperty &source,
                         const PrintingExtrusionScopeProperty &target);

/* Resolve one transition into semantic E-axis and tool-selection requests. */
RetractionDecision decide_boundary(const Config &print_config,
                                   const BoundaryState &source,
                                   const BoundaryState &target,
                                   bool layer_change,
                                   bool actual_toolchange);

/* Configure one empty event with a process role and zero physical flow. */
void set_event_attributes(MutableExtrusionEntity event,
                          raw_extrusion_role role);

/* Append the source-owned semantic Retract request. */
void write_outgoing_retraction(const ScopeEntity &source,
                               const RetractionDecision &decision,
                               const PluginPropertyKey<PrintingExtrusionScopeProperty> &key);

/* Append the target-owned Unretract request after tool-group selection events. */
void write_incoming_retraction(const ScopeEntity &target,
                               const RetractionDecision &decision,
                               const PluginPropertyKey<PrintingExtrusionScopeProperty> &key);

/* Count marked roots for progress without retaining their handles. */
uint32_t scope_count(const PrintingLayerGroup &layer,
                     const PluginPropertyKey<PrintingExtrusionScopeProperty> &key);

/* Append the end-of-print Retract event to the plan after sequence. */
void append_terminal_retraction(storage_handle *storage,
                                const PrintingPlan &plan,
                                double target);

/* Parallel provider that writes retraction operations around scope pairs. */
class CreateRetraction final : public PluginBase
{
public:
    static CreateRetraction &instance(orchestrator_handle *orchestrator);
    explicit CreateRetraction(orchestrator_handle *orchestrator);

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
    void setup_impl(const plugin_run_context *run_ctx,
                    uint32_t run_count) const override;
    void setup_run_impl(const plugin_run_context *run_ctx) const override;
    void run_impl(const plugin_run_context *run_ctx) const override;

    /* Convert final group/layer indexes into the scalar summary array. */
    size_t flat_layer_index(uint32_t group_idx, uint32_t layer_idx) const;

    /* Find the nearest preceding non-empty prepared layer. */
    const LayerBoundarySummary *previous_nonempty_layer(size_t flat_idx) const;

    /* Find the nearest following non-empty prepared layer. */
    const LayerBoundarySummary *next_nonempty_layer(size_t flat_idx) const;

    PluginPropertyKey<PrintingExtrusionScopeProperty> m_scope_property;
    mutable bool m_setup_valid = false;
    mutable std::vector<std::vector<size_t>> m_flat_indices;
    mutable std::vector<LayerBoundarySummary> m_layers;
};

/* Sequential provider that owns the explicit end-of-print boundary. */
class CreateTerminalRetraction final : public PluginBase
{
public:
    static CreateTerminalRetraction &instance(orchestrator_handle *orchestrator);
    explicit CreateTerminalRetraction(orchestrator_handle *orchestrator);

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
    void run_impl(const plugin_run_context *run_ctx) const override;

    PluginPropertyKey<PrintingExtrusionScopeProperty> m_scope_property;
};

coord_t checked_add(const coord_t lhs, const coord_t rhs)
{
    if ((rhs > 0 && lhs > (std::numeric_limits<coord_t>::max)() - rhs) ||
        (rhs < 0 && lhs < (std::numeric_limits<coord_t>::min)() - rhs))
        throw std::overflow_error("A retraction endpoint exceeds coord_t.");
    return lhs + rhs;
}

EffectiveState resolved_state(const ExtrusionEntity &entity,
                              const EffectiveState &parent)
{
    EffectiveState state = parent;
    if (const EPropertyZOffset *z_offset = entity.get(EPropertyZOffset::key))
        state.z_offset = z_offset->get();
    if (const EPropertyModifier *modifier = entity.get(EPropertyModifier::key))
        state.modifier = *modifier;
    return state;
}

bool find_scope_parent_state(MutableExtrusionEntity entity,
                             const extrusion_entity_handle *scope_handle,
                             const EffectiveState &parent,
                             EffectiveState &scope_parent)
{
    if (entity.handle() == scope_handle) {
        scope_parent = parent;
        return true;
    }

    const EffectiveState state = resolved_state(entity.readonly(), parent);
    for (uint32_t child_idx = 0; child_idx < entity.child_count(); ++child_idx)
        if (find_scope_parent_state(entity.child_mutable(child_idx), scope_handle,
                                    state, scope_parent))
            return true;
    return false;
}

bool find_first_geometry(MutableExtrusionEntity entity, const coord_t print_z,
                         const EffectiveState &parent,
                         GeometricEndpoint &endpoint)
{
    const EffectiveState state = resolved_state(entity.readonly(), parent);
    if (entity.segment_count() != 0) {
        const c_extrusion_segment segment = entity.segment(0);
        endpoint.leaf = entity;
        endpoint.position = PlannedPosition{
            segment.point_a.x,
            segment.point_a.y,
            checked_add(checked_add(print_z, state.z_offset), segment.z_offset_a)};
        endpoint.modifier = state.modifier.value_or(EPropertyModifier{});
        const distf_t distance = std::min(
            distf_t(SCALED_EPSILON), entity.local_length() / 2.0);
        if (distance > 0.0) {
            endpoint.interior_point = entity.point_from_begin(distance);
            endpoint.has_interior_point = true;
        }
        return true;
    }
    for (uint32_t child_idx = 0; child_idx < entity.child_count(); ++child_idx)
        if (find_first_geometry(entity.child_mutable(child_idx), print_z,
                                state, endpoint))
            return true;
    return false;
}

bool find_last_geometry(MutableExtrusionEntity entity, const coord_t print_z,
                        const EffectiveState &parent,
                        GeometricEndpoint &endpoint)
{
    const EffectiveState state = resolved_state(entity.readonly(), parent);
    if (entity.segment_count() != 0) {
        const c_extrusion_segment segment = entity.segment(entity.segment_count() - 1);
        endpoint.leaf = entity;
        endpoint.position = PlannedPosition{
            segment.point_b.x,
            segment.point_b.y,
            checked_add(checked_add(print_z, state.z_offset), segment.z_offset_b)};
        endpoint.modifier = state.modifier.value_or(EPropertyModifier{});
        const distf_t distance = std::min(
            distf_t(SCALED_EPSILON), entity.local_length() / 2.0);
        if (distance > 0.0) {
            endpoint.interior_point = entity.point_from_end(distance);
            endpoint.has_interior_point = true;
        }
        return true;
    }
    for (uint32_t child_idx = entity.child_count(); child_idx > 0; --child_idx)
        if (find_last_geometry(entity.child_mutable(child_idx - 1), print_z,
                               state, endpoint))
            return true;
    return false;
}

GeometricEndpoint scope_endpoint(
    const ScopeEntity &scope,
    const PluginPropertyKey<PrintingExtrusionScopeProperty> &key,
    const bool first)
{
    const ExtrusionScope::OrderedExtrusionScope ordered(scope.entity, key);
    EffectiveState scope_parent;
    if (!find_scope_parent_state(scope.printing_extrusion.mutable_root(),
                                 ordered.root().handle(), EffectiveState{}, scope_parent))
        throw std::runtime_error("A retraction scope is outside its PrintingExtrusion.");

    // Wrapped scopes moved their original properties into content. A compact
    // phase-less scope is its own content, so it must inherit from its parent
    // rather than resolving the same direct properties twice.
    MutableExtrusionEntity content = ordered.content();
    const EffectiveState content_parent = content.handle() == ordered.root().handle() ?
        scope_parent : resolved_state(ordered.root().readonly(), scope_parent);
    GeometricEndpoint endpoint;
    const bool found = first ?
        find_first_geometry(content, scope.layer_group.print_z(), content_parent, endpoint) :
        find_last_geometry(content, scope.layer_group.print_z(), content_parent, endpoint);
    if (!found)
        throw std::runtime_error("A prepared retraction scope has no printable geometry.");
    return endpoint;
}

double normal_retract_target(const Config &print_config,
                             const ScopeEntity &source,
                             const GeometricEndpoint &endpoint)
{
    const uint16_t extruder_id = source.tool_group.extruder_id();
    const double printer_target = print_config.vector_float_or_default(
        "retract_length", extruder_id, 0.0);
    const LayerRegionIsland region_island = source.printing_extrusion.region_island();
    if (!region_island.valid() || region_island.region_count() == 0)
        return printer_target;

    const Object object = region_island.region(0).layer().object();
    const uint16_t instance_idx = source.printing_extrusion.object_instance_idx();
    if (instance_idx >= object.instance_count())
        throw std::runtime_error(
            "A retraction source references an invalid object instance.");
    const c_point shift = object.instance_shift(instance_idx);
    c_point lookup_point{
        endpoint.position.x - shift.x,
        endpoint.position.y - shift.y
    };
    RegionSettingsPointResult lookup = RegionSettings::lookup_at_point(
        region_island, k_retraction_region_keys, lookup_point);
    if (!lookup.found() && endpoint.has_interior_point) {
        lookup_point = c_point{
            endpoint.interior_point.x - shift.x,
            endpoint.interior_point.y - shift.y
        };
        lookup = RegionSettings::lookup_at_point(
            region_island, k_retraction_region_keys, lookup_point);
    }
    if (!lookup.found())
        throw std::runtime_error(
            "The retraction source point does not select an unambiguous region.");
    return lookup.value.is_enabled("print_retract_length") ?
        lookup.value.get_float("print_retract_length") : printer_target;
}

BoundaryState boundary_state(
    const ScopeEntity &scope,
    const Config &print_config,
    const PluginPropertyKey<PrintingExtrusionScopeProperty> &key,
    const bool first,
    const bool resolve_normal_target)
{
    const GeometricEndpoint endpoint = scope_endpoint(scope, key, first);
    BoundaryState state;
    state.valid = true;
    state.position = endpoint.position;
    state.modifier = endpoint.modifier;
    state.extruder_id = scope.tool_group.extruder_id();
    if (resolve_normal_target)
        state.normal_retract_target = normal_retract_target(
            print_config, scope, endpoint);
    return state;
}

double planar_gap_mm(const BoundaryState &source,
                     const BoundaryState &target)
{
    const long double dx =
        static_cast<long double>(target.position.x) - source.position.x;
    const long double dy =
        static_cast<long double>(target.position.y) - source.position.y;
    return unscaled(double(std::sqrt(dx * dx + dy * dy)));
}

bool boundary_toolchange(const PrintingExtrusionScopeProperty &source,
                         const PrintingExtrusionScopeProperty &target)
{
    const bool outgoing = printing_extrusion_scope_has_flag(
        source, PRINTING_EXTRUSION_SCOPE_OUTGOING_TOOLCHANGE);
    const bool incoming = printing_extrusion_scope_has_flag(
        target, PRINTING_EXTRUSION_SCOPE_INCOMING_TOOLCHANGE);
    if (outgoing != incoming)
        throw std::runtime_error(
            "Adjacent scopes disagree about their tool-change transition.");
    return outgoing;
}

RetractionDecision decide_boundary(const Config &print_config,
                                   const BoundaryState &source,
                                   const BoundaryState &target,
                                   const bool layer_change,
                                   const bool actual_toolchange)
{
    if (!source.valid || !target.valid)
        throw std::invalid_argument("Retraction decisions need two valid scope endpoints.");

    RetractionDecision decision;
    decision.toolchange_retraction = actual_toolchange ||
        source.modifier.toolchange_retraction != 0 ||
        target.modifier.toolchange_retraction != 0;
    const uint16_t retracting_extruder = source.extruder_id != UINT16_MAX ?
        source.extruder_id : target.extruder_id;
    const double gap = planar_gap_mm(source, target);
    const double minimum_gap = print_config.vector_float_or_default(
        "retract_before_travel", retracting_extruder, 0.0);
    const bool retract_on_layer = print_config.vector_bool_or_default(
        "retract_layer_change", retracting_extruder, false);
    const bool discontinuous = gap >= unscaled(SCALED_EPSILON);

    decision.requested = decision.toolchange_retraction ||
        source.modifier.enforce_retraction != 0 ||
        target.modifier.enforce_retraction != 0 ||
        (discontinuous && gap >= minimum_gap) ||
        (layer_change && retract_on_layer);
    if (source.modifier.disable_retraction != 0 ||
        target.modifier.disable_retraction != 0)
        decision.requested = false;
    if (!decision.requested)
        return decision;

    decision.target = decision.toolchange_retraction ?
        print_config.vector_float_or_default(
            "retract_length_toolchange", retracting_extruder, 0.0) :
        source.normal_retract_target;
    decision.restart_extra = print_config.vector_float_or_default(
        decision.toolchange_retraction ? "retract_restart_extra_toolchange" :
                                         "retract_restart_extra",
        target.extruder_id, 0.0);
    if (!std::isfinite(decision.target) || decision.target < 0.0 ||
        !std::isfinite(decision.restart_extra))
        throw std::runtime_error("Retraction settings produced an invalid E-axis request.");
    return decision;
}

void set_event_attributes(MutableExtrusionEntity event,
                          const raw_extrusion_role role)
{
    EPropertyAttributes &attributes = event.get_or_add(EPropertyAttributes::key);
    attributes.extrusion_role(role)
        .mm3_per_mm(0.0)
        .width(0.f)
        .height(0.f)
        .no_seam_enabled(false);
}

void write_outgoing_retraction(
    const ScopeEntity &source,
    const RetractionDecision &decision,
    const PluginPropertyKey<PrintingExtrusionScopeProperty> &key)
{
    if (!decision.requested || decision.target <= 0.0)
        return;
    const ExtrusionScope::OrderedExtrusionScope scope(source.entity, key);
    MutableExtrusionEntity event = ExtrusionScope::append_phase_leaf(scope.after());
    set_event_attributes(event, RAW_EXTRUSION_ROLE_RETRACT);
    event.get_or_add(EPropertyExtrusionAxis::key).retract_to(
        decision.target, decision.toolchange_retraction);
}

void write_incoming_retraction(
    const ScopeEntity &target,
    const RetractionDecision &decision,
    const PluginPropertyKey<PrintingExtrusionScopeProperty> &key)
{
    const ExtrusionScope::OrderedExtrusionScope scope(target.entity, key);
    if (!decision.requested || decision.target <= 0.0)
        return;

    MutableExtrusionEntity event = ExtrusionScope::append_phase_leaf(scope.before());
    set_event_attributes(event, RAW_EXTRUSION_ROLE_UNRETRACT);
    event.get_or_add(EPropertyExtrusionAxis::key).unretract(
        decision.restart_extra, decision.toolchange_retraction);
}

uint32_t scope_count(
    const PrintingLayerGroup &layer,
    const PluginPropertyKey<PrintingExtrusionScopeProperty> &key)
{
    uint32_t count = 0;
    PrintingEntityPropertyTraversal<PrintingExtrusionScopeProperty> traversal(
        key,
        [&count](ScopeEntity *, ScopeEntity *next) {
            if (next != nullptr)
                ++count;
        },
        MatchingEntityDescendants::Skip);
    traversal.process(layer);
    return count;
}

void append_terminal_retraction(storage_handle *storage,
                                const PrintingPlan &plan,
                                const double target)
{
    if (storage == nullptr || target <= 0.0)
        return;
    StoredExtrusionEntity event(storage);
    set_event_attributes(event.mutable_view(), RAW_EXTRUSION_ROLE_RETRACT);
    event.get_or_add(EPropertyExtrusionAxis::key).retract_to(target, false);
    if (!plan.events().append_after_move(event.mutable_view()).valid())
        throw std::runtime_error("Unable to append the terminal retraction event.");
}

CreateRetraction &CreateRetraction::instance(orchestrator_handle *orchestrator)
{
    static CreateRetraction plugin(orchestrator);
    return plugin;
}

CreateRetraction::CreateRetraction(orchestrator_handle *orchestrator) :
    PluginBase(orchestrator),
    m_scope_property(printing_extrusion_scope_property_key(orchestrator))
{}

const char *CreateRetraction::id_impl() const noexcept
{
    return "layer_extrusion_edit.retraction.default";
}

const char *CreateRetraction::name_impl() const noexcept
{
    return "Create retraction";
}

const char *CreateRetraction::description_impl() const noexcept
{
    return "Adds semantic retract and unretract events to prepared transitions.";
}

const char *CreateRetraction::exclusive_group_impl() const noexcept
{
    return "layer_extrusion_edit.retraction";
}

const char *CreateRetraction::exclusive_group_label_impl() const noexcept
{
    return "Retraction planning";
}

const char *CreateRetraction::exclusive_group_tooltip_impl() const noexcept
{
    return "Selects how semantic retractions are placed around ordered travels.";
}

slicing_step_t CreateRetraction::step_impl() const noexcept
{
    return STEP_LAYER_EXTRUSION_EDIT;
}

const char *const *CreateRetraction::dependencies_impl() const noexcept
{
    return k_scope_dependencies;
}

int32_t CreateRetraction::priority_impl() const noexcept
{
    return -90;
}

int32_t CreateRetraction::used_config_keys(raw_used_config_key *keys) const noexcept
{
    static const raw_used_config_key used_keys[] = {
        {"retract_before_travel", RAW_CO_VECTOR_FLOAT, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE},
        {"retract_layer_change", RAW_CO_VECTOR_BOOL, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE},
        {"retract_length", RAW_CO_VECTOR_FLOAT, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE},
        {"retract_length_toolchange", RAW_CO_VECTOR_FLOAT, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE},
        {"retract_restart_extra", RAW_CO_VECTOR_FLOAT, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE},
        {"retract_restart_extra_toolchange", RAW_CO_VECTOR_FLOAT, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE},
        {"print_retract_length", RAW_CO_FLOAT, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE}
    };
    if (keys != nullptr)
        std::copy(std::begin(used_keys), std::end(used_keys), keys);
    return int32_t(sizeof(used_keys) / sizeof(used_keys[0]));
}

const char *CreateRetraction::progress_message_format_impl() const noexcept
{
    return "Creating retractions: %u / %u scopes";
}

void CreateRetraction::setup_impl(const plugin_run_context *run_ctx,
                                  const uint32_t run_count) const
{
    m_setup_valid = false;
    m_flat_indices.clear();
    m_layers.clear();
    const run_ctx_layer_extrusion_edition *ctx =
        plugin_ctx_as_layer_extrusion_edition(run_ctx);
    if (ctx == nullptr || ctx->plan == nullptr)
        return;

    const PrintingPlan plan(ctx->plan);
    size_t flat_idx = 0;
    m_flat_indices.resize(plan.group_count());
    for (uint32_t group_idx = 0; group_idx < plan.group_count(); ++group_idx) {
        const PrintingGroup group = plan.group(group_idx);
        m_flat_indices[group_idx].resize(group.layer_group_count());
        for (uint32_t layer_idx = 0; layer_idx < group.layer_group_count(); ++layer_idx)
            m_flat_indices[group_idx][layer_idx] = flat_idx++;
    }
    if (flat_idx != run_count)
        throw std::runtime_error("Retraction run count does not match PrintingPlan layers.");
    m_layers.resize(flat_idx);
    m_setup_valid = true;
}

size_t CreateRetraction::flat_layer_index(
    const uint32_t group_idx, const uint32_t layer_idx) const
{
    if (!m_setup_valid || group_idx >= m_flat_indices.size() ||
        layer_idx >= m_flat_indices[group_idx].size())
        throw std::out_of_range("CreateRetraction received an invalid layer index.");
    return m_flat_indices[group_idx][layer_idx];
}

const LayerBoundarySummary *CreateRetraction::previous_nonempty_layer(
    const size_t flat_idx) const
{
    for (size_t idx = flat_idx; idx > 0; --idx) {
        const LayerBoundarySummary &summary = m_layers[idx - 1];
        if (!summary.prepared)
            throw std::runtime_error("A retraction summary was read before the host barrier.");
        if (summary.first.valid)
            return &summary;
    }
    return nullptr;
}

const LayerBoundarySummary *CreateRetraction::next_nonempty_layer(
    const size_t flat_idx) const
{
    for (size_t idx = flat_idx + 1; idx < m_layers.size(); ++idx) {
        const LayerBoundarySummary &summary = m_layers[idx];
        if (!summary.prepared)
            throw std::runtime_error("A retraction summary was read before the host barrier.");
        if (summary.first.valid)
            return &summary;
    }
    return nullptr;
}

void CreateRetraction::setup_run_impl(const plugin_run_context *run_ctx) const
{
    if (!m_setup_valid)
        return;
    const run_ctx_layer_extrusion_edition *ctx =
        plugin_ctx_as_layer_extrusion_edition(run_ctx);
    if (ctx == nullptr || ctx->layer_group == nullptr || ctx->print == nullptr)
        return;

    const PrintingLayerGroup layer(ctx->layer_group);
    const Config print_config = Print(ctx->print).config();
    LayerBoundarySummary summary;
    summary.prepared = true;
    PrintingEntityPropertyTraversal<PrintingExtrusionScopeProperty> traversal(
        m_scope_property,
        [this, &summary, &print_config](ScopeEntity *previous, ScopeEntity *next) {
            if (previous == nullptr && next != nullptr) {
                summary.first = boundary_state(
                    *next, print_config, m_scope_property, true, false);
                summary.first_incoming_toolchange = printing_extrusion_scope_has_flag(
                    *next->property,
                    PRINTING_EXTRUSION_SCOPE_INCOMING_TOOLCHANGE);
            }
            if (previous != nullptr && next == nullptr) {
                summary.last = boundary_state(
                    *previous, print_config, m_scope_property, false, true);
                summary.last_outgoing_toolchange = printing_extrusion_scope_has_flag(
                    *previous->property,
                    PRINTING_EXTRUSION_SCOPE_OUTGOING_TOOLCHANGE);
            }
        },
        MatchingEntityDescendants::Skip);
    traversal.process(layer);
    m_layers[flat_layer_index(ctx->group_idx, ctx->layer_group_idx)] = summary;
    progress().add_max(scope_count(layer, m_scope_property));
}

void CreateRetraction::run_impl(const plugin_run_context *run_ctx) const
{
    if (!m_setup_valid)
        return;
    const run_ctx_layer_extrusion_edition *ctx =
        plugin_ctx_as_layer_extrusion_edition(run_ctx);
    if (ctx == nullptr || ctx->layer_group == nullptr || ctx->print == nullptr)
        return;

    const size_t flat_idx = flat_layer_index(ctx->group_idx, ctx->layer_group_idx);
    if (!m_layers[flat_idx].prepared)
        throw std::runtime_error("A retraction layer has no prepared summary.");
    const LayerBoundarySummary *preceding = previous_nonempty_layer(flat_idx);
    const LayerBoundarySummary *following = next_nonempty_layer(flat_idx);
    const PrintingLayerGroup layer(ctx->layer_group);
    const Config print_config = Print(ctx->print).config();

    PrintingEntityPropertyTraversal<PrintingExtrusionScopeProperty> traversal(
        m_scope_property,
        [this, &print_config, &progress = progress(), preceding, following]
        (ScopeEntity *previous, ScopeEntity *next) {
            if (previous == nullptr && next != nullptr) {
                if (preceding != nullptr) {
                    const bool incoming_toolchange = printing_extrusion_scope_has_flag(
                        *next->property,
                        PRINTING_EXTRUSION_SCOPE_INCOMING_TOOLCHANGE);
                    if (preceding->last_outgoing_toolchange != incoming_toolchange)
                        throw std::runtime_error(
                            "Inter-layer scopes disagree about their tool-change transition.");
                    const BoundaryState target = boundary_state(
                        *next, print_config, m_scope_property, true, false);
                    const RetractionDecision decision = decide_boundary(
                        print_config, preceding->last, target, true,
                        incoming_toolchange);
                    write_incoming_retraction(*next, decision, m_scope_property);
                }
                progress.increment();
                return;
            }

            if (previous != nullptr && next != nullptr) {
                const bool outgoing_transition = printing_extrusion_scope_has_flag(
                    *previous->property,
                    PRINTING_EXTRUSION_SCOPE_OUTGOING_TRANSITION);
                const bool incoming_transition = printing_extrusion_scope_has_flag(
                    *next->property,
                    PRINTING_EXTRUSION_SCOPE_INCOMING_TRANSITION);
                if (outgoing_transition != incoming_transition)
                    throw std::runtime_error(
                        "Adjacent scopes disagree about their retraction transition.");
                if (outgoing_transition) {
                    const BoundaryState source = boundary_state(
                        *previous, print_config, m_scope_property, false, true);
                    const BoundaryState target = boundary_state(
                        *next, print_config, m_scope_property, true, false);
                    const RetractionDecision decision = decide_boundary(
                        print_config, source, target, false,
                        boundary_toolchange(*previous->property, *next->property));
                    write_outgoing_retraction(*previous, decision, m_scope_property);
                    write_incoming_retraction(*next, decision, m_scope_property);
                }
                progress.increment();
                return;
            }

            if (previous != nullptr && following != nullptr) {
                const bool outgoing_toolchange = printing_extrusion_scope_has_flag(
                    *previous->property,
                    PRINTING_EXTRUSION_SCOPE_OUTGOING_TOOLCHANGE);
                if (outgoing_toolchange != following->first_incoming_toolchange)
                    throw std::runtime_error(
                        "Inter-layer scopes disagree about their tool-change transition.");
                const BoundaryState source = boundary_state(
                    *previous, print_config, m_scope_property, false, true);
                const RetractionDecision decision = decide_boundary(
                    print_config, source, following->first, true,
                    outgoing_toolchange);
                write_outgoing_retraction(*previous, decision, m_scope_property);
            }
        },
        MatchingEntityDescendants::Skip);
    traversal.process(layer);
}

CreateTerminalRetraction &CreateTerminalRetraction::instance(
    orchestrator_handle *orchestrator)
{
    static CreateTerminalRetraction plugin(orchestrator);
    return plugin;
}

CreateTerminalRetraction::CreateTerminalRetraction(
    orchestrator_handle *orchestrator) :
    PluginBase(orchestrator),
    m_scope_property(printing_extrusion_scope_property_key(orchestrator))
{}

const char *CreateTerminalRetraction::id_impl() const noexcept
{
    return "extrusion_edit.terminal_retraction.default";
}

const char *CreateTerminalRetraction::name_impl() const noexcept
{
    return "Create terminal retraction";
}

const char *CreateTerminalRetraction::description_impl() const noexcept
{
    return "Adds the final semantic retraction to the PrintingPlan after sequence.";
}

const char *CreateTerminalRetraction::exclusive_group_impl() const noexcept
{
    return "extrusion_edit.terminal_retraction";
}

const char *CreateTerminalRetraction::exclusive_group_label_impl() const noexcept
{
    return "Terminal retraction";
}

const char *CreateTerminalRetraction::exclusive_group_tooltip_impl() const noexcept
{
    return "Selects how the final printable scope leaves filament retracted.";
}

slicing_step_t CreateTerminalRetraction::step_impl() const noexcept
{
    return STEP_EXTRUSION_EDIT;
}

const char *const *CreateTerminalRetraction::dependencies_impl() const noexcept
{
    return k_no_dependencies;
}

int32_t CreateTerminalRetraction::priority_impl() const noexcept
{
    return -100;
}

int32_t CreateTerminalRetraction::used_config_keys(
    raw_used_config_key *keys) const noexcept
{
    static const raw_used_config_key used_keys[] = {
        {"retract_length", RAW_CO_VECTOR_FLOAT, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE},
        {"print_retract_length", RAW_CO_FLOAT, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE}
    };
    if (keys != nullptr)
        std::copy(std::begin(used_keys), std::end(used_keys), keys);
    return int32_t(sizeof(used_keys) / sizeof(used_keys[0]));
}

void CreateTerminalRetraction::run_impl(
    const plugin_run_context *run_ctx) const
{
    const run_ctx_extrusion_edition *ctx =
        plugin_ctx_as_extrusion_edition(run_ctx);
    if (ctx == nullptr || ctx->print == nullptr || ctx->plan == nullptr ||
        run_ctx->plugin_storage == nullptr)
        return;

    const PrintingPlan plan(ctx->plan);
    const Config print_config = Print(ctx->print).config();
    bool found_terminal = false;
    for (uint32_t group_idx = 0; group_idx < plan.group_count(); ++group_idx) {
        const PrintingGroup group = plan.group(group_idx);
        for (uint32_t layer_idx = 0; layer_idx < group.layer_group_count(); ++layer_idx) {
            const PrintingLayerGroup layer = group.layer_group(layer_idx);
            PrintingEntityPropertyTraversal<PrintingExtrusionScopeProperty> traversal(
                m_scope_property,
                [this, &found_terminal, &print_config, &plan, run_ctx]
                (ScopeEntity *, ScopeEntity *next) {
                    if (next == nullptr || !printing_extrusion_scope_has_flag(
                            *next->property, PRINTING_EXTRUSION_SCOPE_TERMINAL))
                        return;
                    if (found_terminal)
                        throw std::runtime_error(
                            "The PrintingPlan contains several terminal extrusion scopes.");
                    found_terminal = true;
                    const BoundaryState source = boundary_state(
                        *next, print_config, m_scope_property, false, true);
                    if (source.modifier.disable_retraction == 0)
                        append_terminal_retraction(
                            run_ctx->plugin_storage, plan,
                            source.normal_retract_target);
                },
                MatchingEntityDescendants::Skip);
            traversal.process(layer);
        }
    }
}

} // namespace

void register_create_retraction_plugins(orchestrator_handle *orchestrator)
{
    orchestrator_register_plugin(
        orchestrator, CreateRetraction::instance(orchestrator).c_instance());
    orchestrator_register_plugin(
        orchestrator, CreateTerminalRetraction::instance(orchestrator).c_instance());
}

}}} // namespace slic3r_api::LayerExtrusionEdit::CreateRetractionPlugin
