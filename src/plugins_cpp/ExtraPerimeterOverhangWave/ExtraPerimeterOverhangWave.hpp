///|/ Copyright (c) SuperSlicer 2026 Durand Rémi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/

#ifndef plugins_cpp_extraperimeteroverhangwave_hpp_
#define plugins_cpp_extraperimeteroverhangwave_hpp_

#include "libslic3r/Api/plugin/cpp/PluginBase.hpp"

namespace slic3r_api { namespace Perimeter { namespace ExtraPerimeterOverhangWavePlugin {

// STEP_POST_PERIMETER alternative for the "extra overhang perimeter" feature.
// It grows printable waves from lower-layer support and clips them to the
// unsupported fill domain. The plugin shares an exclusive group with the
// legacy shrink-based strategy, so only one algorithm owns this feature during
// a slicing run.
class ExtraPerimeterOverhangWave : public PluginBase
{
public:
    static ExtraPerimeterOverhangWave &instance(orchestrator_handle *orch);
    static const char *exclusive_group_ui_fragment() noexcept;

private:
    ExtraPerimeterOverhangWave(orchestrator_handle *orch) : PluginBase(orch) {}

    const char *id_impl() const noexcept override;
    const char *name_impl() const noexcept override;
    const char *description_impl() const noexcept override;
    const char *exclusive_group_impl() const noexcept override;
    const char *exclusive_group_label_impl() const noexcept override;
    const char *exclusive_group_tooltip_impl() const noexcept override;
    slicing_step_t step_impl() const noexcept override;
    const char *const *dependencies_impl() const noexcept override;
    int32_t priority_impl() const noexcept override;
    int32_t used_config_keys(raw_used_config_key *keys) const noexcept override;
    const char *progress_message_format_impl() const noexcept override;
    void inilialize_impl(storage_handle *storage) const override;
    void setup_run_impl(const plugin_run_context *run_ctx) const override;
    void run_impl(const plugin_run_context *run_ctx) const override;
};

void register_extra_perimeter_overhang_wave_plugin(orchestrator_handle *orch);

}}} // namespace slic3r_api::Perimeter::ExtraPerimeterOverhangWavePlugin

#endif // plugins_cpp_extraperimeteroverhangwave_hpp_
