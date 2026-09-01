///|/ Copyright (c) SuperSlicer 2026 Durand Remi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/

/*
Default straight travels
========================

CreateTransitionScope identifies discontinuities and reserves the target
scope's travel phase. This provider selects the direct two-point path while
ScopeTravelConnector handles endpoint resolution, epsilon snapping, Z offsets
and final materialization flags.
*/

#include "DefaultTravel.hpp"

#include <cstdint>
#include <stdexcept>

#include "libslic3r/Api/plugin/c/steps/slic3r_step_layer_extrusion_edit.h"
#include "libslic3r/Api/plugin/cpp/PluginBase.hpp"
#include "libslic3r/Api/plugin/cpp/PrintingPlanViews.hpp"
#include "TravelConnectionHelpers.hpp"

namespace slic3r_api { namespace LayerExtrusionEdit { namespace DefaultTravelPlugin {
namespace {

using namespace TravelConnection;

const char *const k_dependencies[] = {
    "layer_extrusion_edit.transition_scope.default",
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

    mutable ScopeTravelConnector m_connector;
    mutable bool m_setup_valid = false;
};

DefaultTravel &DefaultTravel::instance(orchestrator_handle *orchestrator)
{
    static DefaultTravel plugin(orchestrator);
    return plugin;
}

DefaultTravel::DefaultTravel(orchestrator_handle *orchestrator) :
    PluginBase(orchestrator),
    m_connector(orchestrator)
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

void DefaultTravel::setup_impl(const plugin_run_context *run_ctx, const uint32_t run_count) const
{
    m_connector.reset();
    const run_ctx_layer_extrusion_edition *ctx = plugin_ctx_as_layer_extrusion_edition(run_ctx);
    m_setup_valid = ctx != nullptr && ctx->plan != nullptr;
    if (m_setup_valid)
        m_connector.setup(PrintingPlan(ctx->plan), run_count);
}

void DefaultTravel::setup_run_impl(const plugin_run_context *run_ctx) const
{
    if (!m_setup_valid)
        return;
    const run_ctx_layer_extrusion_edition *ctx = plugin_ctx_as_layer_extrusion_edition(run_ctx);
    if (ctx == nullptr || ctx->layer_group == nullptr)
        return;

    m_connector.setup_run(PrintingLayerGroup(ctx->layer_group),
                          ctx->group_idx, ctx->layer_group_idx, progress());
}

void DefaultTravel::run_impl(const plugin_run_context *run_ctx) const
{
    if (!m_setup_valid)
        return;
    const run_ctx_layer_extrusion_edition *ctx = plugin_ctx_as_layer_extrusion_edition(run_ctx);
    if (ctx == nullptr || ctx->layer_group == nullptr)
        return;

    m_connector.run(PrintingLayerGroup(ctx->layer_group),
                    ctx->group_idx, ctx->layer_group_idx,
                    straight_path, progress());
}

} // namespace

void register_default_travel_plugin(orchestrator_handle *orchestrator)
{
    orchestrator_register_plugin(orchestrator, DefaultTravel::instance(orchestrator).c_instance());
}

}}} // namespace slic3r_api::LayerExtrusionEdit::DefaultTravelPlugin
