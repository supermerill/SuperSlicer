///|/ Copyright (c) SuperSlicer 2026 Durand Remi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/

#include "PostInfillGapFill.hpp"

#include <cassert>

#include "libslic3r/Api/plugin/c/slic3r_orchestrator.h"
#include "libslic3r/Api/plugin/c/steps/slic3r_step_post_infill.h"

/*
Post-infill gap-fill extension point
====================================

This plugin reserves the STEP_POST_INFILL position for gap-fill generation
after the normal infill patterns have run. It is intentionally registered as a
named pipeline stage even though the current context does not yet expose the
residual narrow areas required to generate valid gap-fill extrusion.

The normal call flow is:

    register_post_infill_gap_fill_plugin()
    `-- PostInfillGapFill::instance()
        `-- orchestrator_register_plugin()
            `-- PostInfillGapFill::run_impl()
                |-- read the post-infill generation context
                |-- verify the mutable extrusion callback is available
                `-- leave the existing infill unchanged

No extrusion is appended by the current implementation. When the step context
publishes residual areas and the corresponding output operation, this plugin
is the place where those areas can be converted into additional gap-fill
paths. Until then, the empty run is deliberate and avoids inventing geometry
from incomplete input data.
*/

namespace slic3r_api { namespace Infill { namespace PostInfillGapFillPlugin {
namespace {

const char *k_post_infill_gap_fill_id = "infill.post_process.gap_fill";
const char *k_no_dependencies[] = { nullptr };

} // namespace

PostInfillGapFill &
PostInfillGapFill::instance(orchestrator_handle *orch)
{
    static PostInfillGapFill s_instance(orch);
    return s_instance;
}

const char *PostInfillGapFill::id_impl() const noexcept
{
    return k_post_infill_gap_fill_id;
}

const char *PostInfillGapFill::name_impl() const noexcept
{
    return "Post-infill gap fill";
}

const char *PostInfillGapFill::description_impl() const noexcept
{
    return "Reserved post-infill pass for generating narrow residual gap-fill extrusion after normal infill.";
}

slicing_step_t PostInfillGapFill::step_impl() const noexcept
{
    return STEP_POST_INFILL;
}

const char *const *PostInfillGapFill::dependencies_impl() const noexcept
{
    return k_no_dependencies;
}

int32_t PostInfillGapFill::priority_impl() const noexcept
{
    return 0;
}

void PostInfillGapFill::run_impl(const plugin_run_context *run_ctx) const
{
    const run_ctx_post_infill_generation *ctx = plugin_ctx_as_post_infill_generation(run_ctx);
    assert(ctx != nullptr);
    assert(ctx->get_region_island_mutable_extrusion != nullptr);
    (void) ctx;

    // The plugin is registered now so the pipeline already has a named place
    // for gap-fill-after-infill work. It does not generate extrusion yet:
    // creating good gap fill needs a residual-area input that this step does
    // not publish for the moment. Once that input exists, this plugin will be
    // able to borrow the infill/gap-fill roots from the context and append the
    // new extrusion trees there.
}

void register_post_infill_gap_fill_plugin(orchestrator_handle *orch)
{
    orchestrator_register_plugin(orch, PostInfillGapFill::instance(orch).c_instance());
}

}}} // namespace slic3r_api::Infill::PostInfillGapFillPlugin
