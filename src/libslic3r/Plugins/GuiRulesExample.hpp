///|/ Copyright (c) SuperSlicer 2026 Durand Rémi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/

#ifndef plugins_guirulesexample_hpp_
#define plugins_guirulesexample_hpp_

#include "libslic3r/Api/plugin/cpp/PluginBase.hpp"

namespace slic3r_api { namespace GuiRulesExamplePlugin {

// Built-in example plugin used to exercise plugin-defined GUI rules. It only
// registers settings, a print.ui fragment and activation rules; it does not run
// in the slicing pipeline.
class GuiRulesExample : public PluginBase
{
public:
    static GuiRulesExample &instance(orchestrator_handle *orch);
    static const char *print_ui_fragment() noexcept;

private:
    GuiRulesExample(orchestrator_handle *orch) : PluginBase(orch) {}

    const char *id_impl() const noexcept override;
    const char *name_impl() const noexcept override;
    const char *description_impl() const noexcept override;
    slicing_step_t step_impl() const noexcept override;
    const char *const *dependencies_impl() const noexcept override;
    int32_t priority_impl() const noexcept override;
    int32_t used_config_keys(raw_used_config_key *keys) const noexcept override;
    int32_t defined_config_keys(const char **keys) const noexcept override;
    void inilialize_impl(storage_handle *storage) const override;
    void run_impl(const plugin_run_context *run_ctx) const override;
};

// Register the singleton plugin in an orchestrator or external plugin loader.
void register_gui_rules_example_plugin(orchestrator_handle *orch);

}} // namespace slic3r_api::GuiRulesExamplePlugin

#endif // plugins_guirulesexample_hpp_
