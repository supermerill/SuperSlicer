///|/ Copyright (c) SuperSlicer 2026 Durand Rémi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/

#ifndef plugins_perimeter_detectoverhang_hpp_
#define plugins_perimeter_detectoverhang_hpp_

#include "libslic3r/Api/plugin/cpp/PluginBase.hpp"

namespace slic3r_api { namespace Perimeter { namespace DetectOverhangPlugin {

// STEP_POST_PERIMETER plugin that annotates perimeter fragments whose
// centerline is no longer supported by the previous object layer. It only
// rewrites perimeter extrusion entities; fill-area ownership stays unchanged.
class DetectOverhang : public PluginBase
{
public:
    static DetectOverhang &instance(orchestrator_handle *orch);

private:
    DetectOverhang(orchestrator_handle *orch) : PluginBase(orch) {}

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

void register_detect_overhang_plugin(orchestrator_handle *orch);

}}} // namespace slic3r_api::Perimeter::DetectOverhangPlugin

#endif // plugins_perimeter_detectoverhang_hpp_
