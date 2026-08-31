///|/ Copyright (c) SuperSlicer 2026 Durand Remi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/

#include "DefaultLayerEntryState.hpp"

#include <cstdint>
#include <limits>
#include <stdexcept>
#include <vector>

#include "libslic3r/Api/plugin/c/steps/slic3r_step_layer_extrusion_edit.h"
#include "libslic3r/Api/plugin/cpp/PluginBase.hpp"
#include "libslic3r/Api/plugin/cpp/PrintingPlanViews.hpp"
#include "libslic3r/Api/plugin/cpp/properties/ExtrusionProperties.hpp"
#include "libslic3r/Plugins/PrintingPlan/PrintingLayerEntryStateProperties.h"

namespace slic3r_api { namespace LayerExtrusionEdit { namespace DefaultLayerEntryStatePlugin {
namespace {

/*
Default layer entry-state producer
==================================

The host invokes setup() once and then invokes run() concurrently for every
PrintingLayerGroup. This plugin uses that split deliberately: setup() performs
the only sequential prefix scan, while each run publishes one independent
snapshot into its own layer-group.

The snapshot describes planned PrintingPlan coordinates before begin_layer()
or any synthetic firmware movement. Later parallel plugins may prepend travel
or edit internal paths, but they must preserve the final planned position of
their layer or the already computed state of following layers would be stale.
*/

struct LayerEntryState
{
    PrintingLayerEntryPositionProperty position = {};
    PrintingLayerEntryToolProperty tool = {};
};

const char *k_no_dependencies[] = { nullptr };

/* Add two scaled coordinates and report overflow instead of wrapping Z. */
coord_t checked_add(coord_t lhs, coord_t rhs);

/*
Walk one extrusion tree in execution order and retain its final geometric
position. Direct Z-offset properties replace inherited offsets, matching the
firmware visitor and the PrintingPlan time estimator.
*/
void update_position_from_entity(const ExtrusionEntity &entity,
                                 coord_t print_z,
                                 coord_t inherited_z_offset,
                                 PrintingLayerEntryPositionProperty &position);

/*
Compute one immutable entry snapshot per group/layer index. Empty layers simply
copy the current state, so continuity never requires a parallel worker to read
its predecessor.
*/
std::vector<std::vector<LayerEntryState>> collect_layer_entry_states(const PrintingPlan &plan);

/* Built-in producer registered in the layer-extrusion-edit plugin chain. */
class DefaultLayerEntryState : public PluginBase
{
public:
    static DefaultLayerEntryState &instance(orchestrator_handle *orchestrator);
    explicit DefaultLayerEntryState(orchestrator_handle *orchestrator);

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

    PluginPropertyKey<PrintingLayerEntryPositionProperty> m_position_property;
    PluginPropertyKey<PrintingLayerEntryToolProperty> m_tool_property;
    mutable bool m_setup_valid = false;
    mutable std::vector<std::vector<LayerEntryState>> m_entry_states;
};

coord_t checked_add(const coord_t lhs, const coord_t rhs)
{
    if ((rhs > 0 && lhs > (std::numeric_limits<coord_t>::max)() - rhs) ||
        (rhs < 0 && lhs < (std::numeric_limits<coord_t>::min)() - rhs))
        throw std::overflow_error("A PrintingLayerGroup entry Z position exceeds coord_t.");
    return lhs + rhs;
}

void update_position_from_entity(const ExtrusionEntity &entity,
                                 const coord_t print_z,
                                 const coord_t inherited_z_offset,
                                 PrintingLayerEntryPositionProperty &position)
{
    coord_t z_offset = inherited_z_offset;
    if (const EPropertyZOffset *direct_offset = entity.get(EPropertyZOffset::key))
        z_offset = direct_offset->get();

    // Child order is the final execution order. Passing the resolved offset by
    // value also restores the parent offset automatically between siblings.
    for (uint32_t child_idx = 0; child_idx < entity.child_count(); ++child_idx)
        update_position_from_entity(entity.child(child_idx), print_z, z_offset, position);

    if (entity.segment_count() == 0)
        return;

    const c_extrusion_segment last_segment = entity.segment(entity.segment_count() - 1);
    position.x = last_segment.point_b.x;
    position.y = last_segment.point_b.y;
    position.z = checked_add(checked_add(print_z, z_offset), last_segment.z_offset_b);
    position.state = RAW_PRINTING_LAYER_ENTRY_POSITION_KNOWN;
}

std::vector<std::vector<LayerEntryState>> collect_layer_entry_states(const PrintingPlan &plan)
{
    std::vector<std::vector<LayerEntryState>> states(plan.group_count());
    LayerEntryState current = {};
    current.position.state = RAW_PRINTING_LAYER_ENTRY_POSITION_UNKNOWN;
    current.tool.extruder_id = UINT16_MAX;

    for (uint32_t group_idx = 0; group_idx < plan.group_count(); ++group_idx) {
        const PrintingGroup group = plan.group(group_idx);
        states[group_idx].reserve(group.layer_group_count());
        for (uint32_t layer_idx = 0; layer_idx < group.layer_group_count(); ++layer_idx) {
            const PrintingLayerGroup layer = group.layer_group(layer_idx);

            // Capture before visiting this layer. An empty layer therefore
            // republishes the same state and leaves it available to its successor.
            states[group_idx].push_back(current);

            for (uint32_t tool_idx = 0; tool_idx < layer.tool_group_count(); ++tool_idx) {
                const PrintingToolGroup tool = layer.tool_group(tool_idx);

                // PrintingPlanFileWriter calls begin_tool_group() even for an
                // empty visit, so the selected tool changes independently of geometry.
                current.tool.extruder_id = tool.extruder_id();
                for (uint32_t extrusion_idx = 0; extrusion_idx < tool.extrusion_count(); ++extrusion_idx)
                    update_position_from_entity(
                        tool.extrusion(extrusion_idx).root(), layer.print_z(), 0, current.position);
            }
        }
    }
    return states;
}

DefaultLayerEntryState &DefaultLayerEntryState::instance(orchestrator_handle *orchestrator)
{
    static DefaultLayerEntryState plugin(orchestrator);
    return plugin;
}

DefaultLayerEntryState::DefaultLayerEntryState(orchestrator_handle *orchestrator) :
    PluginBase(orchestrator),
    m_position_property(printing_layer_entry_position_property_key(orchestrator)),
    m_tool_property(printing_layer_entry_tool_property_key(orchestrator))
{}

const char *DefaultLayerEntryState::id_impl() const noexcept
{
    return "layer_extrusion_edit.entry_state.default";
}

const char *DefaultLayerEntryState::name_impl() const noexcept
{
    return "Default layer entry state";
}

const char *DefaultLayerEntryState::description_impl() const noexcept
{
    return "Publishes the planned entry position and active tool of every ordered layer.";
}

const char *DefaultLayerEntryState::exclusive_group_impl() const noexcept
{
    return "layer_extrusion_edit.entry_state";
}

const char *DefaultLayerEntryState::exclusive_group_label_impl() const noexcept
{
    return "Layer entry state";
}

const char *DefaultLayerEntryState::exclusive_group_tooltip_impl() const noexcept
{
    return "Selects how parallel layer editors receive their planned entry state.";
}

slicing_step_t DefaultLayerEntryState::step_impl() const noexcept
{
    return STEP_LAYER_EXTRUSION_EDIT;
}

const char *const *DefaultLayerEntryState::dependencies_impl() const noexcept
{
    return k_no_dependencies;
}

int32_t DefaultLayerEntryState::priority_impl() const noexcept
{
    return -100;
}

const char *DefaultLayerEntryState::progress_message_format_impl() const noexcept
{
    return "Publishing layer entry state: %u / %u layers";
}

void DefaultLayerEntryState::setup_impl(const plugin_run_context *run_ctx, uint32_t) const
{
    m_setup_valid = false;
    m_entry_states.clear();

    const run_ctx_layer_extrusion_edition *ctx = plugin_ctx_as_layer_extrusion_edition(run_ctx);
    if (ctx == nullptr || ctx->plan == nullptr)
        return;

    // Build the complete prefix state before any parallel worker starts. No
    // pointers into layer vectors are retained; runs address the cache by index.
    m_entry_states = collect_layer_entry_states(PrintingPlan(ctx->plan));
    m_setup_valid = true;
}

void DefaultLayerEntryState::setup_run_impl(const plugin_run_context *run_ctx) const
{
    if (!m_setup_valid)
        return;
    const run_ctx_layer_extrusion_edition *ctx = plugin_ctx_as_layer_extrusion_edition(run_ctx);
    if (ctx != nullptr && ctx->layer_group != nullptr)
        progress().add_max(1);
}

void DefaultLayerEntryState::run_impl(const plugin_run_context *run_ctx) const
{
    if (!m_setup_valid)
        return;
    const run_ctx_layer_extrusion_edition *ctx = plugin_ctx_as_layer_extrusion_edition(run_ctx);
    if (ctx == nullptr || ctx->layer_group == nullptr || ctx->group_idx >= m_entry_states.size() ||
        ctx->layer_group_idx >= m_entry_states[ctx->group_idx].size())
        return;

    // Each worker owns one distinct layer-group, so publishing these two
    // payloads needs no lock and never touches a predecessor or successor.
    const LayerEntryState &entry = m_entry_states[ctx->group_idx][ctx->layer_group_idx];
    const PrintingLayerGroup layer(ctx->layer_group);
    PluginProperties properties = layer.properties();
    m_position_property.get_or_add(properties) = entry.position;
    m_tool_property.get_or_add(properties) = entry.tool;
    progress().increment();
}

} // namespace

void register_default_layer_entry_state_plugin(orchestrator_handle *orchestrator)
{
    orchestrator_register_plugin(
        orchestrator, DefaultLayerEntryState::instance(orchestrator).c_instance());
}

}}} // namespace slic3r_api::LayerExtrusionEdit::DefaultLayerEntryStatePlugin
