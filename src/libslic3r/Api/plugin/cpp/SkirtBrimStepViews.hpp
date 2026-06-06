///|/ Copyright (c) SuperSlicer 2026 Durand Rémi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/
#ifndef slic3r_Api_plugin_cpp_SkirtBrimStepViews_hpp_
#define slic3r_Api_plugin_cpp_SkirtBrimStepViews_hpp_

#include <cassert>

#include "libslic3r/Api/plugin/c/steps/slic3r_step_skirt_brim.h"
#include "libslic3r/Api/plugin/cpp/DataTreeViews.hpp"
#include "libslic3r/Api/plugin/cpp/ExtrusionViews.hpp"

namespace slic3r_api {

/*
STEP_SKIRT_BRIM C++ helper
==========================

The skirt/brim step stores its output on Print and PrintObject, not inside the
LayerRegionIsland tree used by perimeters and infill. This helper keeps that
special publication path explicit: a plugin builds an extrusion tree in plugin
storage, then moves it into the host with append_*_move().

Moving means ownership of the extrusion content is transferred to the host. The
StoredExtrusionEntity object itself remains valid, but callers should treat its
tree as consumed after a successful append.
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

    bool clear_brim() const
    {
        assert(m_ctx->clear_brim != nullptr);
        return m_ctx->clear_brim(m_ctx->print) != 0;
    }

    bool clear_object_brim(const Object &object) const
    {
        assert(m_ctx->clear_object_brim != nullptr);
        return m_ctx->clear_object_brim(const_cast<object_handle *>(object.handle())) != 0;
    }

    bool append_brim_move(StoredExtrusionEntity &extrusion) const
    {
        assert(m_ctx->append_brim_move != nullptr);
        return m_ctx->append_brim_move(m_ctx->print, extrusion.mutable_handle()) != 0;
    }

    bool append_object_brim_move(const Object &object, StoredExtrusionEntity &extrusion) const
    {
        assert(m_ctx->append_object_brim_move != nullptr);
        return m_ctx->append_object_brim_move(
            const_cast<object_handle *>(object.handle()),
            extrusion.mutable_handle()) != 0;
    }

private:
    const run_ctx_skirt_brim *m_ctx = nullptr;
};

} // namespace slic3r_api

#endif // slic3r_Api_plugin_cpp_SkirtBrimStepViews_hpp_
