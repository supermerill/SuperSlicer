///|/ Copyright (c) SuperSlicer 2026 Durand Rémi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/

#ifndef plugins_perimeter_extraperimetersonoverhangs_hpp_
#define plugins_perimeter_extraperimetersonoverhangs_hpp_

#include "libslic3r/Api/plugin/cpp/PluginBase.hpp"

namespace slic3r_api { namespace Perimeter { namespace ExtraPerimetersOnOverhangsPlugin {

// STEP_POST_PERIMETER plugin that adds local overhang perimeter paths after
// the main perimeter generator has produced the normal perimeter tree. The
// generated paths are inserted before the normal perimeters so they can anchor
// into supported plastic before the rest of the island is printed.
class ExtraPerimetersOnOverhangs : public PluginBase
{
public:
    static ExtraPerimetersOnOverhangs &instance(orchestrator_handle *orch);

private:
    ExtraPerimetersOnOverhangs(orchestrator_handle *orch) : PluginBase(orch) {}

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

void register_extra_perimeters_on_overhangs_plugin(orchestrator_handle *orch);

}}} // namespace slic3r_api::Perimeter::ExtraPerimetersOnOverhangsPlugin

#endif // plugins_perimeter_extraperimetersonoverhangs_hpp_
