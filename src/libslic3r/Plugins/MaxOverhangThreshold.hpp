///|/ Copyright (c) SuperSlicer 2026 Durand Rémi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/

#ifndef plugins_maxoverhangthreshold_hpp_
#define plugins_maxoverhangthreshold_hpp_

#include "libslic3r/Api/plugin/cpp/PluginBase.hpp"

namespace slic3r_api { namespace MaxOverhangThresholdPlugin {

// Official post-slicing plugin that reproduces the legacy
// PrintObject::_max_overhang_threshold() pass through the public plugin API.
// It edits raw LayerRegion slices, then asks the host to rebuild the dependent
// layer slice/island caches.
class MaxOverhangThreshold : public PluginBase
{
public:
    static MaxOverhangThreshold &instance(orchestrator_handle *orch);
    static const char *print_ui_fragment() noexcept;

private:
    MaxOverhangThreshold(orchestrator_handle *orch) : PluginBase(orch) {}
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

// Register the singleton plugin in an orchestrator or external plugin loader.
void register_max_overhang_threshold_plugin(orchestrator_handle *orch);

}} // namespace slic3r_api::MaxOverhangThresholdPlugin

#endif // plugins_maxoverhangthreshold_hpp_
