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

#include <cstdint>
#include <stdexcept>

#include "libslic3r/Api/plugin/c/steps/slic3r_step_layer_extrusion_edit.h"
#include "libslic3r/Api/plugin/cpp/PluginBase.hpp"
#include "libslic3r/Api/plugin/cpp/PrintingPlanViews.hpp"
#include "libslic3r/Plugins/PrintingPlan/PrintingLayerEntryStateProperties.h"

#include "TravelConnectionHelpers.hpp"

namespace slic3r_api { namespace LayerExtrusionEdit { namespace DefaultTravelPlugin {
namespace {

using namespace TravelConnection;

const char *const k_dependencies[] = {
    "layer_extrusion_edit.entry_state.default",
    nullptr
};

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
    add_layer_progress(layer, progress());
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
    connect_layer(layer, position, straight_path, progress());
}

} // namespace

void register_default_travel_plugin(orchestrator_handle *orchestrator)
{
    orchestrator_register_plugin(orchestrator, DefaultTravel::instance(orchestrator).c_instance());
}

}}} // namespace slic3r_api::LayerExtrusionEdit::DefaultTravelPlugin
