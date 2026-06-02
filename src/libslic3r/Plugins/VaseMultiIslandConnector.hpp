///|/ Copyright (c) SuperSlicer 2026 Durand Remi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/

#ifndef plugins_vasemultiislandconnector_hpp_
#define plugins_vasemultiislandconnector_hpp_

#include "libslic3r/Api/plugin/cpp/PluginBase.hpp"

namespace slic3r_api { namespace VaseMultiIslandConnectorPlugin {

/*
VaseMultiIslandConnector
========================

Spiral vase mode can only print one continuous outer path per layer. A sliced
object may still contain several separate islands on the same layer, for
example when the model branches and reconnects. This post-slicing plugin edits
the raw slices before perimeter generation so the later vase-mode perimeter
generator sees a single printable island whenever nearby islands can be joined
with real material.

The plugin works on LayerRegion raw slices. After it writes the replacement
region geometry, it asks the host to rebuild the layer slices and
LayerSliceIsland list from those regions. This keeps the normal post-slicing
ownership model: regions own the editable raw geometry, while layer islands are
derived caches for later pipeline steps.
*/
class VaseMultiIslandConnector : public PluginBase
{
public:
    static VaseMultiIslandConnector &instance(orchestrator_handle *orch);

private:
    explicit VaseMultiIslandConnector(orchestrator_handle *orch) : PluginBase(orch) {}

    const char *id_impl() const noexcept override;
    const char *name_impl() const noexcept override;
    const char *description_impl() const noexcept override;
    slicing_step_t step_impl() const noexcept override;
    const char *const *dependencies_impl() const noexcept override;
    int32_t priority_impl() const noexcept override;
    int32_t used_config_keys(raw_used_config_key *keys) const noexcept override;
    const char *progress_message_format_impl() const noexcept override;
    void setup_run_impl(const plugin_run_context *run_ctx) const override;
    void run_impl(const plugin_run_context *run_ctx) const override;
};

// Register the built-in singleton in an orchestrator or test plugin runtime.
void register_vase_multi_island_connector_plugin(orchestrator_handle *orch);

}} // namespace slic3r_api::VaseMultiIslandConnectorPlugin

#endif // plugins_vasemultiislandconnector_hpp_
