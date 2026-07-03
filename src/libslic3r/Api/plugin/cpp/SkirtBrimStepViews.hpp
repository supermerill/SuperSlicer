///|/ Copyright (c) SuperSlicer 2026 Durand Rémi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/
#ifndef slic3r_Api_plugin_cpp_SkirtBrimStepViews_hpp_
#define slic3r_Api_plugin_cpp_SkirtBrimStepViews_hpp_

#include <cassert>

#include "libslic3r/Api/plugin/c/steps/slic3r_step_skirt_brim.h"
#include "libslic3r/Api/plugin/cpp/DataTreeViews.hpp"

namespace slic3r_api {

/*
STEP_SKIRT_BRIM C++ helper
==========================

The step itself only gives plugins a mutable Print handle. Brim and skirt are
ordinary auxiliary layers now: create them on print.auxiliary_object() for
global adhesion or on a real Object for object-local adhesion, tag the layer
with LayerAdhesionProperty, and write extrusions into its LayerRegionIslands.
*/
class SkirtBrimStep
{
public:
    explicit SkirtBrimStep(const run_ctx_skirt_brim *ctx) : m_ctx(ctx) { assert(m_ctx != nullptr); }
    explicit SkirtBrimStep(const plugin_run_context *run_ctx)
        : SkirtBrimStep(plugin_ctx_as_skirt_brim(run_ctx)) {}

    Print print() const
    {
        assert(m_ctx->print != nullptr);
        return Print(reinterpret_cast<const print_handle *>(m_ctx->print));
    }

private:
    const run_ctx_skirt_brim *m_ctx = nullptr;
};

} // namespace slic3r_api

#endif // slic3r_Api_plugin_cpp_SkirtBrimStepViews_hpp_
