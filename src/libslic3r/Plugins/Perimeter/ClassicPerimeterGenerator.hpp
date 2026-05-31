///|/ Copyright (c) SuperSlicer 2026 Durand Rémi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/

#ifndef plugins_perimeter_classicperimetergenerator_hpp_
#define plugins_perimeter_classicperimetergenerator_hpp_

#include "libslic3r/Api/plugin/cpp/PluginBase.hpp"

namespace slic3r_api { namespace Perimeter { namespace ClassicPerimeterGeneratorPlugin {

// STEP_PERIMETER skeleton for the future API-only transcription of
// PerimeterGenerator::process_classic(). It is intentionally conservative for
// now: the host-owned perimeter tree is already used, while the classic-specific
// blocks are documented in the implementation and will be filled in gradually.
class ClassicPerimeterGenerator : public PluginBase
{
public:
    static ClassicPerimeterGenerator &instance(orchestrator_handle *orch);

private:
    ClassicPerimeterGenerator(orchestrator_handle *orch) : PluginBase(orch) {}

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

void register_classic_perimeter_generator_plugin(orchestrator_handle *orch);

}}} // namespace slic3r_api::Perimeter::ClassicPerimeterGeneratorPlugin

#endif // plugins_perimeter_classicperimetergenerator_hpp_
