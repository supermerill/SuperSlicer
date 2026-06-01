///|/ Copyright (c) SuperSlicer 2026 Durand Rémi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/

#ifndef plugins_perimeter_markfirstloop_hpp_
#define plugins_perimeter_markfirstloop_hpp_

#include "libslic3r/Api/plugin/cpp/PluginBase.hpp"

namespace slic3r_api { namespace Perimeter { namespace MarkFirstLoopPlugin {

// PerimeterGenerationModule that marks the innermost generated perimeter loops.
//
// The module runs after the perimeter tree has been fully generated. At that
// point it can inspect parent/child relationships instead of guessing from
// geometry. A loop receives FIRST_LOOP only when no child branch below the same
// perimeter node contains another taggable loop.
class MarkFirstLoop : public PluginBase
{
public:
    static MarkFirstLoop &instance(orchestrator_handle *orch);

private:
    MarkFirstLoop(orchestrator_handle *orch) : PluginBase(orch) {}

    const char *id_impl() const noexcept override;
    slicing_step_t step_impl() const noexcept override;
    const char *const *dependencies_impl() const noexcept override;
    int32_t priority_impl() const noexcept override;
    const char *progress_message_format_impl() const noexcept override;
    void run_impl(const plugin_run_context *run_ctx) const override;
};

void register_mark_first_loop_plugin(orchestrator_handle *orch);

}}} // namespace slic3r_api::Perimeter::MarkFirstLoopPlugin

#endif // plugins_perimeter_markfirstloop_hpp_
