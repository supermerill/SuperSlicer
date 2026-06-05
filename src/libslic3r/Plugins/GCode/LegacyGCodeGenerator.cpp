///|/ Copyright (c) SuperSlicer 2026 Durand Rémi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/

#include "LegacyGCodeGenerator.hpp"

#include "libslic3r/Api/plugin/c/slic3r_orchestrator.h"
#include "libslic3r/Api/plugin/cpp/PluginBase.hpp"
#include "libslic3r/libslic3r.h"

/*
Legacy G-code selector
======================

STEP_GCODE is now an exclusive plugin step. Most plugins in that step receive a
PrintingPlan and write the output file through the plugin API. The legacy G-code
generator is different: it is still the original host-side GCodeGenerator, so
calling it through the plugin run() callback would be the wrong abstraction.

This file registers a small built-in plugin only to reserve a selectable value
in the STEP_GCODE combo box. Orchestrator::export_gcode() detects that value and
routes to the legacy host implementation before the plugin pipeline is entered.
*/

namespace slic3r_api { namespace GCodeGeneration { namespace LegacyGCodeGeneratorPlugin {
namespace {

const char *const k_no_dependencies[] = { nullptr };

class LegacyGCodeGenerator : public PluginBase
{
public:
    static LegacyGCodeGenerator &instance(orchestrator_handle *orch)
    {
        static LegacyGCodeGenerator s_instance(orch);
        return s_instance;
    }

    explicit LegacyGCodeGenerator(orchestrator_handle *orch) : PluginBase(orch) {}

private:
    const char *id_impl() const noexcept override { return "gcode.legacy"; }
    const char *name_impl() const noexcept override { return "Legacy G-code generator"; }
    const char *description_impl() const noexcept override
    {
        return "Uses the original host GCodeGenerator export path.";
    }
    const char *exclusive_group_impl() const noexcept override { return "step_gcode_plugin"; }
    const char *exclusive_group_label_impl() const noexcept override { return "G-code plugin"; }
    const char *exclusive_group_tooltip_impl() const noexcept override
    {
        return "Selects how the final G-code file is generated.";
    }
    slicing_step_t step_impl() const noexcept override { return STEP_GCODE; }
    const char *const *dependencies_impl() const noexcept override { return k_no_dependencies; }
    int32_t priority_impl() const noexcept override { return -100; }

    void inilialize_impl(storage_handle *) const override
    {
        /*
        Place the STEP_GCODE selector next to firmware flavor because both
        options describe the G-code dialect/output backend. The fragment id is
        the exclusive group id, so the generic print.ui fallback can detect that
        the selector already has an explicit home.
        */
        orchestrator_add_ui_fragment(
            m_orchestrator,
            "printer_fff.ui",
            "step_gcode_plugin",
            "page:General:printer\n"
            "group:Firmware\n"
            "\tsetting:insert$beforesetting$gcode_flavor:step_gcode_plugin\n",
            0);
    }

    void run_impl(const plugin_run_context *) const override
    {
        /*
        A direct STEP_GCODE run means the caller bypassed
        Orchestrator::export_gcode(), where the legacy branch is handled. Fail
        loudly so a test or external host does not silently produce no output.
        */
        throw Slic3r::RuntimeError(
            "Legacy G-code generator is handled directly by Orchestrator::export_gcode().");
    }
};

} // namespace

void register_legacy_gcode_generator_plugin(orchestrator_handle *orch)
{
    orchestrator_register_plugin(orch, LegacyGCodeGenerator::instance(orch).c_instance());
}

}}} // namespace slic3r_api::GCodeGeneration::LegacyGCodeGeneratorPlugin
