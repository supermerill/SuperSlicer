///|/ Copyright (c) SuperSlicer 2026 Durand Rémi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/

#ifndef plugins_support_supportdemandbridgeremoval_hpp_
#define plugins_support_supportdemandbridgeremoval_hpp_

#include "libslic3r/Api/plugin/cpp/PluginBase.hpp"

namespace slic3r_api { namespace Support { namespace SupportDemandBridgeRemovalPlugin {

// STEP_SUPPORT_DEMAND refinement plugin.
//
// It runs after overhangs, support painting and support modifier volumes. When
// dont_support_bridges is enabled, it removes support demand under straight
// extrusion spans whose ends are both supported by the lower layer.
class SupportDemandBridgeRemoval : public PluginBase
{
public:
    static SupportDemandBridgeRemoval &instance(orchestrator_handle *orch);
    static const char *print_ui_fragment() noexcept;

private:
    SupportDemandBridgeRemoval(orchestrator_handle *orch) : PluginBase(orch) {}

    const char *id_impl() const noexcept override;
    const char *name_impl() const noexcept override;
    const char *description_impl() const noexcept override;
    slicing_step_t step_impl() const noexcept override;
    const char *const *dependencies_impl() const noexcept override;
    int32_t priority_impl() const noexcept override;
    int32_t used_config_keys(raw_used_config_key *keys) const noexcept override;
    int32_t defined_config_keys(const char **keys) const noexcept override;
    const char *progress_message_format_impl() const noexcept override;
    void inilialize_impl(storage_handle *storage) const override;
    void setup_run_impl(const plugin_run_context *run_ctx) const override;
    void run_impl(const plugin_run_context *run_ctx) const override;
};

void register_support_demand_bridge_removal_plugin(orchestrator_handle *orch);

}}} // namespace slic3r_api::Support::SupportDemandBridgeRemovalPlugin

#endif // plugins_support_supportdemandbridgeremoval_hpp_
