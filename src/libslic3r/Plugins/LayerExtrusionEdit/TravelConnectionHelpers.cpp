///|/ Copyright (c) SuperSlicer 2026 Durand Remi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/

/*
Compact-scope travel connection implementation
==============================================

CreateTransitionScope has already marked maximal printable scopes and reserved
a travel phase before each real incoming transition. This module streams those
roots, resolves their endpoints and fills only the reserved phase.

Layer workers remain independent. setup_run() publishes a scalar prediction
for the first scope. After the host barrier, the preceding layer uses that fact
to update its own final outgoing flag without mutating the next layer.
*/

#include "TravelConnectionHelpers.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <stdexcept>

#include "ExtrusionScopeHelpers.hpp"
#include "libslic3r/Api/plugin/cpp/PluginBase.hpp"
#include "libslic3r/Api/plugin/cpp/PrintingEntityPropertyTraversal.hpp"
#include "libslic3r/Api/plugin/cpp/properties/ExtrusionProperties.hpp"

namespace slic3r_api { namespace LayerExtrusionEdit { namespace TravelConnection {
namespace {

using ScopeEntity = PrintingEntity<PrintingExtrusionScopeProperty>;

/* One endpoint leaf with the Z offset effective at that leaf. */
struct GeometricLeaf
{
    MutableExtrusionEntity entity;
    coord_t effective_z_offset = 0;
    PlannedPosition first;
    PlannedPosition last;
    bool closed = false;
};

/* Add scaled coordinates without signed overflow. */
coord_t checked_add(coord_t lhs, coord_t rhs);

/* Subtract scaled coordinates without signed overflow. */
coord_t checked_subtract(coord_t lhs, coord_t rhs);

/* Measure a three-dimensional gap in scaled plan coordinates. */
long double scaled_distance(const PlannedPosition &lhs, const PlannedPosition &rhs);

/* Resolve one local segment endpoint into plan coordinates. */
PlannedPosition absolute_position(c_point point, coord_t print_z,
                                  coord_t effective_z_offset, coord_t point_z_offset);

/* Find the first geometric descendant and resolve its inherited Z. */
bool find_first_geometry(MutableExtrusionEntity entity, coord_t print_z,
                         coord_t inherited_z_offset, GeometricLeaf &result);

/* Find the last geometric descendant and resolve its inherited Z. */
bool find_last_geometry(MutableExtrusionEntity entity, coord_t print_z,
                        coord_t inherited_z_offset, GeometricLeaf &result);

/* Build one planner endpoint, including regional provenance and an inner probe. */
TravelEndpoint endpoint_from_leaf(const GeometricLeaf &leaf,
                                  const PrintingExtrusion &owner, bool from_begin);

/* Snap a target seam without destroying arc metadata. */
void snap_leaf_start(GeometricLeaf leaf,
                     const PlannedPosition &position, coord_t print_z);

/* Interpolate absolute endpoint Z over a provider-selected planar path. */
std::vector<coord_t> interpolated_z_offsets(const std::vector<c_point> &path,
                                            const PlannedPosition &from,
                                            const PlannedPosition &to,
                                            coord_t base_z);

/* Fill an empty reserved phase with one explicit Travel leaf. */
void define_travel_leaf(MutableExtrusionEntity travel,
                        const std::vector<c_point> &path,
                        const PlannedPosition &from,
                        const PlannedPosition &to,
                        coord_t print_z, coord_t inherited_z_offset);

/* Validate and report a travel already present in the reserved phase. */
bool validate_materialized_travel(const ExtrusionScope::OrderedExtrusionScope &scope);

/* Read the planned machine position before one layer starts. */
PlannedPosition layer_entry_position(
    const PrintingLayerGroup &layer,
    const PluginPropertyKey<PrintingLayerEntryPositionProperty> &key);

/* Count marked roots for progress without retaining a flattened list. */
uint32_t scope_count(const PrintingLayerGroup &layer,
                     const PluginPropertyKey<PrintingExtrusionScopeProperty> &key);

/* Record each marked root's parent Z contribution. */
void collect_scope_z_contexts(
    MutableExtrusionEntity entity, coord_t inherited_z_offset,
    const PluginPropertyKey<PrintingExtrusionScopeProperty> &key,
    std::map<const extrusion_entity_handle *, coord_t> &contexts);

/* Build the layer-local inherited-Z lookup. */
std::map<const extrusion_entity_handle *, coord_t> scope_z_contexts(
    const PrintingLayerGroup &layer,
    const PluginPropertyKey<PrintingExtrusionScopeProperty> &key);

/* Read one marked root's inherited-Z entry. */
coord_t scope_z_context(
    const ScopeEntity &scope,
    const std::map<const extrusion_entity_handle *, coord_t> &contexts);

/* Resolve the last geometry from after, or from printable content. */
GeometricLeaf source_geometry(const ScopeEntity &source,
    const ExtrusionScope::OrderedExtrusionScope &scope, coord_t inherited_z_offset);

/* Resolve the first geometry from before, or from printable content. */
GeometricLeaf target_geometry(const ScopeEntity &target,
    const ExtrusionScope::OrderedExtrusionScope &scope, coord_t inherited_z_offset);

/* Connect one preceding scope or layer entry to a target scope. */
bool connect_to_scope(ScopeEntity *source, ScopeEntity &target,
    const PlannedPosition &layer_entry,
    const std::map<const extrusion_entity_handle *, coord_t> &z_contexts,
    const PluginPropertyKey<PrintingExtrusionScopeProperty> &key,
    const TravelPathPlanner &planner);

/* Predict the first scope's final materialized bit before the run barrier. */
bool predict_first_materialized(ScopeEntity &first,
    const PlannedPosition &layer_entry,
    const std::map<const extrusion_entity_handle *, coord_t> &z_contexts,
    const PluginPropertyKey<PrintingExtrusionScopeProperty> &key);

coord_t checked_add(const coord_t lhs, const coord_t rhs)
{
    if ((rhs > 0 && lhs > (std::numeric_limits<coord_t>::max)() - rhs) ||
        (rhs < 0 && lhs < (std::numeric_limits<coord_t>::min)() - rhs))
        throw std::overflow_error("A travel endpoint exceeds the coord_t range.");
    return lhs + rhs;
}

coord_t checked_subtract(const coord_t lhs, const coord_t rhs)
{
    if ((rhs > 0 && lhs < (std::numeric_limits<coord_t>::min)() + rhs) ||
        (rhs < 0 && lhs > (std::numeric_limits<coord_t>::max)() + rhs))
        throw std::overflow_error("A travel Z offset exceeds the coord_t range.");
    return lhs - rhs;
}

long double scaled_distance(const PlannedPosition &lhs, const PlannedPosition &rhs)
{
    const long double dx = static_cast<long double>(rhs.x) - lhs.x;
    const long double dy = static_cast<long double>(rhs.y) - lhs.y;
    const long double dz = static_cast<long double>(rhs.z) - lhs.z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

PlannedPosition absolute_position(const c_point point, const coord_t print_z,
                                  const coord_t effective_z_offset,
                                  const coord_t point_z_offset)
{
    PlannedPosition result;
    result.x = point.x;
    result.y = point.y;
    result.z = checked_add(checked_add(print_z, effective_z_offset), point_z_offset);
    result.known = true;
    return result;
}

bool find_first_geometry(MutableExtrusionEntity entity, const coord_t print_z,
                         const coord_t inherited_z_offset, GeometricLeaf &result)
{
    coord_t effective_z_offset = inherited_z_offset;
    if (const EPropertyZOffset *direct = entity.get(EPropertyZOffset::key))
        effective_z_offset = direct->get();

    if (entity.segment_count() != 0) {
        const c_extrusion_segment first = entity.segment(0);
        const c_extrusion_segment last = entity.segment(entity.segment_count() - 1);
        result.entity = entity;
        result.effective_z_offset = effective_z_offset;
        result.first = absolute_position(first.point_a, print_z,
                                         effective_z_offset, first.z_offset_a);
        result.last = absolute_position(last.point_b, print_z,
                                        effective_z_offset, last.z_offset_b);
        result.closed = entity.point_count() > 1 &&
            result.first.x == result.last.x && result.first.y == result.last.y &&
            result.first.z == result.last.z;
        return true;
    }
    for (uint32_t idx = 0; idx < entity.child_count(); ++idx) {
        if (find_first_geometry(entity.child_mutable(idx), print_z,
                                effective_z_offset, result))
            return true;
    }
    return false;
}

bool find_last_geometry(MutableExtrusionEntity entity, const coord_t print_z,
                        const coord_t inherited_z_offset, GeometricLeaf &result)
{
    coord_t effective_z_offset = inherited_z_offset;
    if (const EPropertyZOffset *direct = entity.get(EPropertyZOffset::key))
        effective_z_offset = direct->get();

    if (entity.segment_count() != 0) {
        const c_extrusion_segment first = entity.segment(0);
        const c_extrusion_segment last = entity.segment(entity.segment_count() - 1);
        result.entity = entity;
        result.effective_z_offset = effective_z_offset;
        result.first = absolute_position(first.point_a, print_z,
                                         effective_z_offset, first.z_offset_a);
        result.last = absolute_position(last.point_b, print_z,
                                        effective_z_offset, last.z_offset_b);
        result.closed = entity.point_count() > 1 &&
            result.first.x == result.last.x && result.first.y == result.last.y &&
            result.first.z == result.last.z;
        return true;
    }
    for (uint32_t idx = entity.child_count(); idx > 0; --idx) {
        if (find_last_geometry(entity.child_mutable(idx - 1), print_z,
                               effective_z_offset, result))
            return true;
    }
    return false;
}

TravelEndpoint endpoint_from_leaf(const GeometricLeaf &leaf,
                                  const PrintingExtrusion &owner,
                                  const bool from_begin)
{
    TravelEndpoint endpoint;
    endpoint.position = from_begin ? leaf.first : leaf.last;
    endpoint.region_island = owner.region_island();
    endpoint.object_instance_idx = owner.object_instance_idx();
    const distf_t length = leaf.entity.length();
    const distf_t distance = std::min(distf_t(SCALED_EPSILON), length / 2.0);
    if (distance > 0.0) {
        endpoint.interior_point = from_begin ?
            leaf.entity.point_from_begin(distance) : leaf.entity.point_from_end(distance);
        endpoint.has_interior_point = true;
    }
    return endpoint;
}

void snap_leaf_start(GeometricLeaf leaf,
                     const PlannedPosition &position, const coord_t print_z)
{
    const coord_t base_z = checked_add(print_z, leaf.effective_z_offset);
    const coord_t z_offset = checked_subtract(position.z, base_z);
    const c_point point{position.x, position.y};
    // Editing segments preserves arc centers, unlike the simpler point setter.
    std::vector<c_extrusion_segment> segments = leaf.entity.segments();
    if (segments.empty())
        throw std::runtime_error("Unable to snap an extrusion leaf without segments.");
    segments.front().point_a = point;
    segments.front().z_offset_a = z_offset;
    if (leaf.closed) {
        segments.back().point_b = point;
        segments.back().z_offset_b = z_offset;
    }
    if (!leaf.entity.set_segments(segments))
        throw std::runtime_error("Unable to snap an extrusion while preserving its arcs.");
}

std::vector<coord_t> interpolated_z_offsets(const std::vector<c_point> &path,
                                            const PlannedPosition &from,
                                            const PlannedPosition &to,
                                            const coord_t base_z)
{
    std::vector<long double> cumulative(path.size(), 0.0L);
    for (size_t idx = 1; idx < path.size(); ++idx) {
        const long double dx = static_cast<long double>(path[idx].x) - path[idx - 1].x;
        const long double dy = static_cast<long double>(path[idx].y) - path[idx - 1].y;
        cumulative[idx] = cumulative[idx - 1] + std::sqrt(dx * dx + dy * dy);
    }
    std::vector<coord_t> offsets(path.size(), checked_subtract(from.z, base_z));
    const long double total = cumulative.back();
    for (size_t idx = 1; idx + 1 < path.size(); ++idx) {
        const long double ratio = total > 0.0L ? cumulative[idx] / total : 0.0L;
        const long double z = static_cast<long double>(from.z) +
            (static_cast<long double>(to.z) - from.z) * ratio;
        if (z < static_cast<long double>((std::numeric_limits<coord_t>::min)()) ||
            z > static_cast<long double>((std::numeric_limits<coord_t>::max)()))
            throw std::overflow_error("An interpolated travel Z exceeds coord_t.");
        offsets[idx] = checked_subtract(coord_t(std::llround(z)), base_z);
    }
    offsets.back() = checked_subtract(to.z, base_z);
    return offsets;
}

void define_travel_leaf(MutableExtrusionEntity travel,
                        const std::vector<c_point> &path,
                        const PlannedPosition &from,
                        const PlannedPosition &to,
                        const coord_t print_z,
                        const coord_t inherited_z_offset)
{
    if (!travel.valid() || travel.child_count() != 0 || travel.segment_count() != 0)
        throw std::runtime_error("A reserved travel phase must be an empty leaf.");
    if (path.size() < 2 || path.front().x != from.x || path.front().y != from.y ||
        path.back().x != to.x || path.back().y != to.y)
        throw std::runtime_error("A travel planner must preserve both endpoints.");

    const coord_t base_z = checked_add(print_z, inherited_z_offset);
    const std::vector<coord_t> offsets = interpolated_z_offsets(path, from, to, base_z);
    if (!travel.set_points(path))
        throw std::runtime_error("Unable to define reserved travel geometry.");
    for (uint32_t idx = 0; idx < uint32_t(path.size()); ++idx) {
        if (!travel.set_z_offset(idx, offsets[idx]))
            throw std::runtime_error("Unable to define a reserved travel Z offset.");
    }
    EPropertyAttributes &attributes = travel.get_or_add(EPropertyAttributes::key);
    attributes.extrusion_role(RAW_EXTRUSION_ROLE_TRAVEL)
              .mm3_per_mm(0.0).width(0.f).height(0.f).no_seam_enabled(false);
}

bool validate_materialized_travel(const ExtrusionScope::OrderedExtrusionScope &scope)
{
    const MutableExtrusionEntity travel = scope.travel();
    if (!travel.valid() || (travel.segment_count() == 0 && travel.child_count() == 0))
        return false;
    if (travel.child_count() != 0 || travel.segment_count() == 0)
        throw std::runtime_error("A reserved travel must be one geometric leaf.");
    const EPropertyAttributes *attributes = travel.get(EPropertyAttributes::key);
    if (attributes == nullptr || !RAW_EXTRUSION_ROLE_IS_TRAVEL(attributes->extrusion_role()))
        throw std::runtime_error("A non-empty travel phase must carry the Travel role.");
    return true;
}

PlannedPosition layer_entry_position(
    const PrintingLayerGroup &layer,
    const PluginPropertyKey<PrintingLayerEntryPositionProperty> &key)
{
    const PrintingLayerEntryPositionProperty *entry = key.get(layer.properties());
    if (entry == nullptr)
        throw std::runtime_error("Travel generation requires the layer entry position.");
    if (entry->state != RAW_PRINTING_LAYER_ENTRY_POSITION_UNKNOWN &&
        entry->state != RAW_PRINTING_LAYER_ENTRY_POSITION_KNOWN)
        throw std::runtime_error("The layer entry position has an invalid state.");
    PlannedPosition position;
    if (entry->is_known()) {
        position.x = entry->x;
        position.y = entry->y;
        position.z = entry->z;
        position.known = true;
    }
    return position;
}

uint32_t scope_count(const PrintingLayerGroup &layer,
                     const PluginPropertyKey<PrintingExtrusionScopeProperty> &key)
{
    uint32_t count = 0;
    PrintingEntityPropertyTraversal<PrintingExtrusionScopeProperty> traversal(
        key, [&count](ScopeEntity *, ScopeEntity *next) {
            if (next != nullptr)
                ++count;
        }, MatchingEntityDescendants::Skip);
    traversal.process(layer);
    return count;
}

void collect_scope_z_contexts(
    MutableExtrusionEntity entity, const coord_t inherited_z_offset,
    const PluginPropertyKey<PrintingExtrusionScopeProperty> &key,
    std::map<const extrusion_entity_handle *, coord_t> &contexts)
{
    // Store the parent contribution first: a phase-less scope may itself carry Z.
    if (entity.get(key) != nullptr)
        contexts.emplace(entity.handle(), inherited_z_offset);
    coord_t child_z_offset = inherited_z_offset;
    if (const EPropertyZOffset *direct = entity.get(EPropertyZOffset::key))
        child_z_offset = direct->get();
    for (uint32_t idx = 0; idx < entity.child_count(); ++idx)
        collect_scope_z_contexts(entity.child_mutable(idx), child_z_offset, key, contexts);
}

std::map<const extrusion_entity_handle *, coord_t> scope_z_contexts(
    const PrintingLayerGroup &layer,
    const PluginPropertyKey<PrintingExtrusionScopeProperty> &key)
{
    std::map<const extrusion_entity_handle *, coord_t> contexts;
    for (uint32_t tool_idx = 0; tool_idx < layer.tool_group_count(); ++tool_idx) {
        const PrintingToolGroup tool = layer.tool_group(tool_idx);
        for (uint32_t idx = 0; idx < tool.extrusion_count(); ++idx)
            collect_scope_z_contexts(tool.extrusion(idx).mutable_root(), 0, key, contexts);
    }
    return contexts;
}

coord_t scope_z_context(
    const ScopeEntity &scope,
    const std::map<const extrusion_entity_handle *, coord_t> &contexts)
{
    const std::map<const extrusion_entity_handle *, coord_t>::const_iterator found =
        contexts.find(scope.entity.handle());
    if (found == contexts.end())
        throw std::runtime_error("A traversed scope has no inherited Z context.");
    return found->second;
}

GeometricLeaf source_geometry(const ScopeEntity &source,
    const ExtrusionScope::OrderedExtrusionScope &scope,
    const coord_t inherited_z_offset)
{
    GeometricLeaf result;
    const MutableExtrusionEntity after = scope.after();
    if (after.valid() && find_last_geometry(after, source.layer_group.print_z(),
                                            inherited_z_offset, result))
        return result;
    if (!find_last_geometry(scope.content(), source.layer_group.print_z(),
                            inherited_z_offset, result))
        throw std::runtime_error("An extrusion scope has no source geometry.");
    return result;
}

GeometricLeaf target_geometry(const ScopeEntity &target,
    const ExtrusionScope::OrderedExtrusionScope &scope,
    const coord_t inherited_z_offset)
{
    GeometricLeaf result;
    const MutableExtrusionEntity before = scope.before();
    if (before.valid() && find_first_geometry(before, target.layer_group.print_z(),
                                              inherited_z_offset, result))
        return result;
    if (!find_first_geometry(scope.content(), target.layer_group.print_z(),
                             inherited_z_offset, result))
        throw std::runtime_error("An extrusion scope has no target geometry.");
    return result;
}

bool connect_to_scope(ScopeEntity *source, ScopeEntity &target,
    const PlannedPosition &layer_entry,
    const std::map<const extrusion_entity_handle *, coord_t> &z_contexts,
    const PluginPropertyKey<PrintingExtrusionScopeProperty> &key,
    const TravelPathPlanner &planner)
{
    ExtrusionScope::OrderedExtrusionScope target_scope(target.entity, key);
    const bool incoming = target_scope.has_incoming_transition();
    if (source != nullptr) {
        const ExtrusionScope::OrderedExtrusionScope source_scope(source->entity, key);
        if (source_scope.has_outgoing_transition() != incoming)
            throw std::runtime_error("Adjacent scopes disagree about their transition.");
    }
    const bool marked = printing_extrusion_scope_has_flag(
        *target.property, PRINTING_EXTRUSION_SCOPE_INCOMING_TRAVEL_MATERIALIZED);
    const bool stored = incoming && validate_materialized_travel(target_scope);
    if (marked || stored)
        return true;

    const coord_t target_z = scope_z_context(target, z_contexts);
    const GeometricLeaf target_leaf = target_geometry(target, target_scope, target_z);
    const TravelEndpoint target_endpoint = endpoint_from_leaf(
        target_leaf, target.printing_extrusion, true);
    TravelEndpoint source_endpoint;
    if (source != nullptr) {
        const ExtrusionScope::OrderedExtrusionScope source_scope(source->entity, key);
        const GeometricLeaf source_leaf = source_geometry(
            *source, source_scope, scope_z_context(*source, z_contexts));
        source_endpoint = endpoint_from_leaf(source_leaf, source->printing_extrusion, false);
    } else {
        source_endpoint.position = layer_entry;
    }
    if (!source_endpoint.position.known) {
        if (incoming)
            throw std::runtime_error("An incoming transition has no known source position.");
        return false;
    }

    const long double gap = scaled_distance(source_endpoint.position, target_endpoint.position);
    if (gap < static_cast<long double>(SCALED_EPSILON)) {
        snap_leaf_start(target_leaf, source_endpoint.position, target.layer_group.print_z());
        return false;
    }
    if (!incoming)
        throw std::runtime_error("Discontinuous scopes have no reserved travel phase.");
    const std::vector<c_point> path = planner(
        source_endpoint, target_endpoint, target.tool_group.extruder_id());
    define_travel_leaf(target_scope.travel(), path, source_endpoint.position,
                       target_endpoint.position, target.layer_group.print_z(), target_z);
    return true;
}

bool predict_first_materialized(ScopeEntity &first,
    const PlannedPosition &layer_entry,
    const std::map<const extrusion_entity_handle *, coord_t> &z_contexts,
    const PluginPropertyKey<PrintingExtrusionScopeProperty> &key)
{
    const ExtrusionScope::OrderedExtrusionScope scope(first.entity, key);
    if (!scope.has_incoming_transition())
        return false;
    if (printing_extrusion_scope_has_flag(
            *first.property, PRINTING_EXTRUSION_SCOPE_INCOMING_TRAVEL_MATERIALIZED) ||
        validate_materialized_travel(scope))
        return true;
    if (!layer_entry.known)
        throw std::runtime_error("An incoming layer transition has no entry position.");
    const GeometricLeaf target = target_geometry(
        first, scope, scope_z_context(first, z_contexts));
    return scaled_distance(layer_entry, target.first) >=
        static_cast<long double>(SCALED_EPSILON);
}

} // namespace

/* Register the private properties shared with scope and entry-state plugins. */
ScopeTravelConnector::ScopeTravelConnector(orchestrator_handle *orchestrator) :
    m_scope_property(printing_extrusion_scope_property_key(orchestrator)),
    m_entry_position_property(printing_layer_entry_position_property_key(orchestrator))
{}

/* Clear mappings and summaries from a previous plan. */
void ScopeTravelConnector::reset()
{
    m_setup_valid = false;
    m_flat_indices.clear();
    m_layers.clear();
}

/* Build a stable layer index without scanning extrusion geometry. */
void ScopeTravelConnector::setup(const PrintingPlan &plan, const uint32_t run_count)
{
    reset();
    if (!plan.valid())
        throw std::invalid_argument("ScopeTravelConnector needs a valid PrintingPlan.");
    size_t flat_idx = 0;
    m_flat_indices.resize(plan.group_count());
    for (uint32_t group_idx = 0; group_idx < plan.group_count(); ++group_idx) {
        const PrintingGroup group = plan.group(group_idx);
        m_flat_indices[group_idx].resize(group.layer_group_count());
        for (uint32_t layer_idx = 0; layer_idx < group.layer_group_count(); ++layer_idx)
            m_flat_indices[group_idx][layer_idx] = flat_idx++;
    }
    if (flat_idx != run_count)
        throw std::runtime_error("Travel run count does not match PrintingPlan layers.");
    m_layers.resize(flat_idx);
    m_setup_valid = true;
}

/* Convert final group/layer indexes into one summary slot. */
size_t ScopeTravelConnector::flat_layer_index(
    const uint32_t group_idx, const uint32_t layer_idx) const
{
    if (!m_setup_valid || group_idx >= m_flat_indices.size() ||
        layer_idx >= m_flat_indices[group_idx].size())
        throw std::out_of_range("Travel provider received an invalid layer index.");
    return m_flat_indices[group_idx][layer_idx];
}

/* Find the next prepared layer containing a scope. */
const ScopeTravelConnector::LayerBoundarySummary *
ScopeTravelConnector::next_nonempty_layer(const size_t flat_idx) const
{
    for (size_t idx = flat_idx + 1; idx < m_layers.size(); ++idx) {
        if (!m_layers[idx].prepared)
            throw std::runtime_error("A travel summary was read before the host barrier.");
        if (m_layers[idx].has_scope)
            return &m_layers[idx];
    }
    return nullptr;
}

/* Publish the first local scope's final incoming-travel prediction. */
void ScopeTravelConnector::setup_run(const PrintingLayerGroup &layer,
    const uint32_t group_idx, const uint32_t layer_idx, PluginProgress &progress)
{
    const size_t flat_idx = flat_layer_index(group_idx, layer_idx);
    LayerBoundarySummary summary;
    summary.prepared = true;
    const PlannedPosition entry = layer_entry_position(layer, m_entry_position_property);
    const std::map<const extrusion_entity_handle *, coord_t> contexts =
        scope_z_contexts(layer, m_scope_property);
    PrintingEntityPropertyTraversal<PrintingExtrusionScopeProperty> traversal(
        m_scope_property,
        [this, &summary, &entry, &contexts](ScopeEntity *previous, ScopeEntity *next) {
            if (previous == nullptr && next != nullptr) {
                summary.has_scope = true;
                summary.first_has_incoming_transition = printing_extrusion_scope_has_flag(
                    *next->property, PRINTING_EXTRUSION_SCOPE_INCOMING_TRANSITION);
                summary.first_travel_materialized_after_run = predict_first_materialized(
                    *next, entry, contexts, m_scope_property);
            }
        }, MatchingEntityDescendants::Skip);
    traversal.process(layer);
    m_layers[flat_idx] = summary;
    progress.add_max(scope_count(layer, m_scope_property));
}

/* Connect local pairs and synchronize only flags owned by this layer. */
void ScopeTravelConnector::run(const PrintingLayerGroup &layer,
    const uint32_t group_idx, const uint32_t layer_idx,
    const TravelPathPlanner &planner, PluginProgress &progress) const
{
    if (!planner)
        throw std::invalid_argument("ScopeTravelConnector needs a path planner.");
    const size_t flat_idx = flat_layer_index(group_idx, layer_idx);
    const LayerBoundarySummary &summary = m_layers[flat_idx];
    if (!summary.prepared)
        throw std::runtime_error("Travel run has no prepared layer summary.");
    if (!summary.has_scope)
        return;

    const PlannedPosition entry = layer_entry_position(layer, m_entry_position_property);
    const std::map<const extrusion_entity_handle *, coord_t> contexts =
        scope_z_contexts(layer, m_scope_property);
    const LayerBoundarySummary *following = next_nonempty_layer(flat_idx);
    PrintingEntityPropertyTraversal<PrintingExtrusionScopeProperty> traversal(
        m_scope_property,
        [this, &entry, &contexts, &planner, &progress, following]
        (ScopeEntity *previous, ScopeEntity *next) {
            if (next != nullptr) {
                const bool materialized = connect_to_scope(
                    previous, *next, entry, contexts, m_scope_property, planner);
                PrintingExtrusionScopeProperty *target = next->entity.get_mutable(m_scope_property);
                if (target == nullptr)
                    throw std::runtime_error("A target scope lost its marker.");
                printing_extrusion_scope_set_flag(*target,
                    PRINTING_EXTRUSION_SCOPE_INCOMING_TRAVEL_MATERIALIZED, materialized);
                if (previous != nullptr) {
                    PrintingExtrusionScopeProperty *source =
                        previous->entity.get_mutable(m_scope_property);
                    if (source == nullptr)
                        throw std::runtime_error("A source scope lost its marker.");
                    printing_extrusion_scope_set_flag(*source,
                        PRINTING_EXTRUSION_SCOPE_OUTGOING_TRAVEL_MATERIALIZED, materialized);
                }
                progress.increment();
                return;
            }
            if (previous != nullptr) {
                const bool has_outgoing = printing_extrusion_scope_has_flag(
                    *previous->property, PRINTING_EXTRUSION_SCOPE_OUTGOING_TRANSITION);
                const bool materialized = following != nullptr ?
                    following->first_travel_materialized_after_run : false;
                if ((following != nullptr &&
                     following->first_has_incoming_transition != has_outgoing) ||
                    (following == nullptr && has_outgoing))
                    throw std::runtime_error("Inter-layer scopes disagree about their transition.");
                PrintingExtrusionScopeProperty *source =
                    previous->entity.get_mutable(m_scope_property);
                if (source == nullptr)
                    throw std::runtime_error("A final scope lost its marker.");
                printing_extrusion_scope_set_flag(*source,
                    PRINTING_EXTRUSION_SCOPE_OUTGOING_TRAVEL_MATERIALIZED, materialized);
            }
        }, MatchingEntityDescendants::Skip);
    traversal.process(layer);
}

/* Return the direct path used by the simple provider and advanced fallbacks. */
std::vector<c_point> straight_path(const TravelEndpoint &source,
                                   const TravelEndpoint &target, uint16_t)
{
    return {c_point{source.position.x, source.position.y},
            c_point{target.position.x, target.position.y}};
}

}}} // namespace slic3r_api::LayerExtrusionEdit::TravelConnection
