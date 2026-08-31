///|/ Copyright (c) SuperSlicer 2026 Durand Remi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/

/*
Default straight travels
========================

PrintingPlan stores the final order of extrusion trees, but independently
generated leaves need not share endpoints. This plugin turns that implicit gap
into an explicit Travel leaf. Each parallel run starts from the entry position
published by DefaultLayerEntryState, then follows only its own layer-group.

Tiny gaps below SCALED_EPSILON are removed by snapping the next start point.
Larger gaps use emplace_ordered_leaf() so the original tree remains intact and
its properties continue to describe only its previous content.
*/

#include "DefaultTravel.hpp"

#include <cmath>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <vector>

#include "libslic3r/Api/plugin/c/steps/slic3r_step_layer_extrusion_edit.h"
#include "libslic3r/Api/plugin/cpp/ExtrusionViews.hpp"
#include "libslic3r/Api/plugin/cpp/PluginBase.hpp"
#include "libslic3r/Api/plugin/cpp/PrintingPlanViews.hpp"
#include "libslic3r/Api/plugin/cpp/properties/ExtrusionProperties.hpp"
#include "libslic3r/Plugins/PrintingPlan/PrintingLayerEntryStateProperties.h"

namespace slic3r_api { namespace LayerExtrusionEdit { namespace DefaultTravelPlugin {
namespace {

struct PlannedPosition
{
    coord_t x = 0;
    coord_t y = 0;
    coord_t z = 0;
    bool known = false;
};

const char *const k_dependencies[] = {
    "layer_extrusion_edit.entry_state.default",
    nullptr
};

/* Add two scaled coordinates without allowing signed overflow. */
coord_t checked_add(coord_t lhs, coord_t rhs);

/* Subtract two scaled coordinates without allowing signed overflow. */
coord_t checked_subtract(coord_t lhs, coord_t rhs);

/* Resolve one point into absolute PrintingPlan XYZ coordinates. */
PlannedPosition absolute_position(c_point point,
                                  coord_t print_z,
                                  coord_t entity_z_offset,
                                  coord_t point_z_offset);

/* Return the scaled three-dimensional distance without overflowing coord_t. */
long double scaled_distance(const PlannedPosition &lhs, const PlannedPosition &rhs);

/* Detect a genuinely closed 3D leaf before a possible seam snap. */
bool leaf_is_closed(const MutableExtrusionEntity &entity,
                    const PlannedPosition &first,
                    const PlannedPosition &last);

/*
Move a sub-epsilon start onto the preceding endpoint. Closed leaves move both
copies of their seam so their first and last points remain identical.
*/
void snap_leaf_start(MutableExtrusionEntity entity,
                     const PlannedPosition &position,
                     coord_t print_z,
                     coord_t entity_z_offset,
                     bool closed);

/*
Create the explicit straight connector before one leaf. The new leaf inherits
the parent Z offset, while the old leaf and its direct properties are moved
under the preserved-content sibling created by emplace_ordered_leaf().
*/
void prepend_travel(MutableExtrusionEntity entity,
                    const PlannedPosition &from,
                    const PlannedPosition &to,
                    coord_t print_z,
                    coord_t parent_z_offset);

/* Traverse one tree in final execution order and connect every geometric leaf. */
void connect_entity(MutableExtrusionEntity entity,
                    coord_t print_z,
                    coord_t inherited_z_offset,
                    PlannedPosition &position);

/* Process every extrusion tree of one independently editable layer-group. */
void connect_layer(const PrintingLayerGroup &layer, PlannedPosition &position, PluginProgress &progress);

class DefaultTravel : public PluginBase
{
public:
    static DefaultTravel &instance(orchestrator_handle *orchestrator);
    explicit DefaultTravel(orchestrator_handle *orchestrator);

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
    const char *progress_message_format_impl() const noexcept override;
    void setup_impl(const plugin_run_context *run_ctx, uint32_t run_count) const override;
    void setup_run_impl(const plugin_run_context *run_ctx) const override;
    void run_impl(const plugin_run_context *run_ctx) const override;

    PluginPropertyKey<PrintingLayerEntryPositionProperty> m_entry_position_property;
    mutable bool m_setup_valid = false;
};

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

PlannedPosition absolute_position(const c_point point,
                                  const coord_t print_z,
                                  const coord_t entity_z_offset,
                                  const coord_t point_z_offset)
{
    PlannedPosition position = {};
    position.x = point.x;
    position.y = point.y;
    position.z = checked_add(checked_add(print_z, entity_z_offset), point_z_offset);
    position.known = true;
    return position;
}

long double scaled_distance(const PlannedPosition &lhs, const PlannedPosition &rhs)
{
    const long double dx = static_cast<long double>(rhs.x) - static_cast<long double>(lhs.x);
    const long double dy = static_cast<long double>(rhs.y) - static_cast<long double>(lhs.y);
    const long double dz = static_cast<long double>(rhs.z) - static_cast<long double>(lhs.z);
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

bool leaf_is_closed(const MutableExtrusionEntity &entity,
                    const PlannedPosition &first,
                    const PlannedPosition &last)
{
    return entity.point_count() > 1 && first.x == last.x && first.y == last.y && first.z == last.z;
}

void snap_leaf_start(MutableExtrusionEntity entity,
                     const PlannedPosition &position,
                     const coord_t print_z,
                     const coord_t entity_z_offset,
                     const bool closed)
{
    const coord_t base_z = checked_add(print_z, entity_z_offset);
    const coord_t snapped_z_offset = checked_subtract(position.z, base_z);
    const c_point snapped_point{position.x, position.y};

    // Rebuild from true segments instead of set_point(), whose local-edit
    // contract intentionally linearizes adjacent arcs. Only the seam endpoints
    // change; every radius, orientation, intermediate point, and direct
    // extrusion property remains intact.
    std::vector<c_extrusion_segment> segments = entity.segments();
    if (segments.empty())
        throw std::runtime_error("Unable to snap an extrusion leaf without segments.");
    segments.front().point_a = snapped_point;
    segments.front().z_offset_a = snapped_z_offset;

    if (closed) {
        segments.back().point_b = snapped_point;
        segments.back().z_offset_b = snapped_z_offset;
    }
    if (!entity.set_segments(segments))
        throw std::runtime_error("Unable to snap an extrusion start while preserving its segments.");
}

void prepend_travel(MutableExtrusionEntity entity,
                    const PlannedPosition &from,
                    const PlannedPosition &to,
                    const coord_t print_z,
                    const coord_t parent_z_offset)
{
    MutableExtrusionEntity travel = entity.emplace_ordered_leaf(
        OrderedLeafPosition::Before,
        ExistingPropertyPlacement::MoveWithExistingContent);
    if (!travel.valid())
        throw std::runtime_error("Unable to insert an ordered travel before an extrusion leaf.");

    // The connector is a sibling of the preserved old content, so express its
    // endpoint Z offsets relative to the parent state it actually inherits.
    const coord_t base_z = checked_add(print_z, parent_z_offset);
    const std::vector<c_point> points{
        c_point{from.x, from.y},
        c_point{to.x, to.y}
    };
    if (!travel.set_points(points) ||
        !travel.set_z_offset(0, checked_subtract(from.z, base_z)) ||
        !travel.set_z_offset(1, checked_subtract(to.z, base_z)))
        throw std::runtime_error("Unable to define the straight travel geometry.");

    // Travel carries its role directly because MoveWithExistingContent keeps
    // the old leaf's attributes with the old leaf. Zero flow prevents any
    // material extrusion while later plugins still provide speed/acceleration.
    EPropertyAttributes &attributes = travel.get_or_add(EPropertyAttributes::key);
    attributes.extrusion_role(RAW_EXTRUSION_ROLE_TRAVEL)
              .mm3_per_mm(0.0)
              .width(0.f)
              .height(0.f)
              .no_seam_enabled(false);
}

void connect_entity(MutableExtrusionEntity entity,
                    const coord_t print_z,
                    const coord_t inherited_z_offset,
                    PlannedPosition &position)
{
    coord_t entity_z_offset = inherited_z_offset;
    if (const EPropertyZOffset *direct_z = entity.get(EPropertyZOffset::key))
        entity_z_offset = direct_z->get();

    // Collections contain the authoritative execution order. Each child sees
    // the same inherited Z state, while position advances between siblings.
    if (entity.child_count() > 0) {
        const uint32_t child_count = entity.child_count();
        for (uint32_t child_idx = 0; child_idx < child_count; ++child_idx)
            connect_entity(entity.child_mutable(child_idx), print_z, entity_z_offset, position);
        return;
    }
    if (entity.segment_count() == 0)
        return;

    // Snapshot endpoints before a structural insertion turns this leaf into a
    // wrapper. The preserved child keeps all original segments without cloning.
    const c_extrusion_segment first_segment = entity.segment(0);
    const c_extrusion_segment last_segment = entity.segment(entity.segment_count() - 1);
    PlannedPosition first = absolute_position(
        first_segment.point_a, print_z, entity_z_offset, first_segment.z_offset_a);
    PlannedPosition last = absolute_position(
        last_segment.point_b, print_z, entity_z_offset, last_segment.z_offset_b);
    const bool closed = leaf_is_closed(entity, first, last);

    if (position.known) {
        const long double gap = scaled_distance(position, first);
        if (gap < static_cast<long double>(SCALED_EPSILON)) {
            snap_leaf_start(entity, position, print_z, entity_z_offset, closed);
            first = position;
            if (closed)
                last = position;
        } else {
            prepend_travel(entity, position, first, print_z, inherited_z_offset);
        }
    }

    // The first geometric leaf establishes position without a synthetic move.
    // Every later leaf either starts exactly here or now has a connector.
    position = last;
}

void connect_layer(const PrintingLayerGroup &layer,
                   PlannedPosition &position,
                   PluginProgress &progress)
{
    for (uint32_t tool_idx = 0; tool_idx < layer.tool_group_count(); ++tool_idx) {
        const PrintingToolGroup tool = layer.tool_group(tool_idx);
        for (uint32_t extrusion_idx = 0; extrusion_idx < tool.extrusion_count(); ++extrusion_idx) {
            connect_entity(tool.extrusion(extrusion_idx).mutable_root(), layer.print_z(), 0, position);
            progress.increment();
        }
    }
}

DefaultTravel &DefaultTravel::instance(orchestrator_handle *orchestrator)
{
    static DefaultTravel plugin(orchestrator);
    return plugin;
}

DefaultTravel::DefaultTravel(orchestrator_handle *orchestrator) :
    PluginBase(orchestrator),
    m_entry_position_property(printing_layer_entry_position_property_key(orchestrator))
{}

const char *DefaultTravel::id_impl() const noexcept
{
    return "layer_extrusion_edit.travel.default";
}

const char *DefaultTravel::name_impl() const noexcept
{
    return "Default straight travel";
}

const char *DefaultTravel::description_impl() const noexcept
{
    return "Connects ordered extrusion leaves with simple straight travels.";
}

const char *DefaultTravel::exclusive_group_impl() const noexcept
{
    return "layer_extrusion_edit.travel";
}

const char *DefaultTravel::exclusive_group_label_impl() const noexcept
{
    return "Travel generation";
}

const char *DefaultTravel::exclusive_group_tooltip_impl() const noexcept
{
    return "Selects how discontinuities between ordered extrusion leaves become travels.";
}

slicing_step_t DefaultTravel::step_impl() const noexcept
{
    return STEP_LAYER_EXTRUSION_EDIT;
}

const char *const *DefaultTravel::dependencies_impl() const noexcept
{
    return k_dependencies;
}

int32_t DefaultTravel::priority_impl() const noexcept
{
    return -50;
}

const char *DefaultTravel::progress_message_format_impl() const noexcept
{
    return "Connecting ordered extrusion paths: %u / %u trees";
}

void DefaultTravel::setup_impl(const plugin_run_context *run_ctx, uint32_t) const
{
    const run_ctx_layer_extrusion_edition *ctx = plugin_ctx_as_layer_extrusion_edition(run_ctx);
    m_setup_valid = ctx != nullptr && ctx->plan != nullptr;
}

void DefaultTravel::setup_run_impl(const plugin_run_context *run_ctx) const
{
    if (!m_setup_valid)
        return;
    const run_ctx_layer_extrusion_edition *ctx = plugin_ctx_as_layer_extrusion_edition(run_ctx);
    if (ctx == nullptr || ctx->layer_group == nullptr)
        return;

    const PrintingLayerGroup layer(ctx->layer_group);
    uint32_t extrusion_count = 0;
    for (uint32_t tool_idx = 0; tool_idx < layer.tool_group_count(); ++tool_idx)
        extrusion_count += layer.tool_group(tool_idx).extrusion_count();
    progress().add_max(extrusion_count);
}

void DefaultTravel::run_impl(const plugin_run_context *run_ctx) const
{
    if (!m_setup_valid)
        return;
    const run_ctx_layer_extrusion_edition *ctx = plugin_ctx_as_layer_extrusion_edition(run_ctx);
    if (ctx == nullptr || ctx->layer_group == nullptr)
        return;

    const PrintingLayerGroup layer(ctx->layer_group);
    const PrintingLayerEntryPositionProperty *entry =
        m_entry_position_property.get(layer.properties());
    if (entry == nullptr)
        throw std::runtime_error(
            "Straight travel generation requires the PrintingLayerGroup entry-position property.");
    if (entry->state != RAW_PRINTING_LAYER_ENTRY_POSITION_UNKNOWN &&
        entry->state != RAW_PRINTING_LAYER_ENTRY_POSITION_KNOWN)
        throw std::runtime_error("PrintingLayerGroup entry position has an invalid state.");

    PlannedPosition position = {};
    if (entry->is_known()) {
        position.x = entry->x;
        position.y = entry->y;
        position.z = entry->z;
        position.known = true;
    }
    connect_layer(layer, position, progress());
}

} // namespace

void register_default_travel_plugin(orchestrator_handle *orchestrator)
{
    orchestrator_register_plugin(orchestrator, DefaultTravel::instance(orchestrator).c_instance());
}

}}} // namespace slic3r_api::LayerExtrusionEdit::DefaultTravelPlugin
