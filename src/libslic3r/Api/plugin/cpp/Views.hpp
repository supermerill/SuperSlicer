///|/ Copyright (c) SuperSlicer 2026 Durand Rémi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/
#ifndef slic3r_Api_plugin_cpp_Views_hpp_
#define slic3r_Api_plugin_cpp_Views_hpp_

/*
Plugin C++ view index
=====================

This header is the convenient aggregate include for the C++ plugin API. It
exposes the views used to inspect and modify plugin-visible configuration,
geometry, data-tree objects, extrusions, orchestration services, and printing
plans. The individual headers remain the more precise choice when a plugin
needs only one part of the API.

The general ownership rule is the same throughout the view layer:

    host-owned data   -> borrowed view, no destruction responsibility
    plugin-built data -> Stored* object in plugin storage, move ownership when
                         a step callback publishes it

Borrowed views are lightweight and convenient, but they do not keep native
objects alive. Reacquire them after the host rebuilds or replaces the structure
they refer to. `Stored*` objects own temporary storage-backed data and are
move-only; they are used to assemble a complete result before transferring it
to the host.

The one small data wrapper defined directly here is `LayerConfigRanges`. It
borrows the array of layer configuration ranges from the current layer-height
run context. Its elements are valid only while that context and its range array
remain alive; the wrapper must not be retained for a later slicing step.

Typical include and traversal setup:

    #include "libslic3r/Api/plugin/cpp/Views.hpp"

    Print print(ctx->print);
    Object object = print.object(0);
    Layer layer = object.layer(0);
    LayerIsland island = layer.island(0);

Use the specialized view headers and their module documentation for the exact
mutation, storage, and invalidation contract of each type.
*/

#include <cassert>

#include "libslic3r/Api/plugin/c/steps/slic3r_step_layer_height.h"
#include "libslic3r/Api/plugin/cpp/BridgeDetectorViews.hpp"
#include "libslic3r/Api/plugin/cpp/ClipperViews.hpp"
#include "libslic3r/Api/plugin/cpp/DataTreeViews.hpp"
#include "libslic3r/Api/plugin/cpp/SurfaceViews.hpp"
#include "libslic3r/Api/plugin/cpp/ExtrusionViews.hpp"
#include "libslic3r/Api/plugin/cpp/GeometryViews.hpp"
#include "libslic3r/Api/plugin/cpp/LineDistancer.hpp"
#include "libslic3r/Api/plugin/cpp/OrchestratorViews.hpp"
#include "libslic3r/Api/plugin/cpp/PluginBase.hpp"
#include "libslic3r/Api/plugin/cpp/PluginContext.hpp"
#include "libslic3r/Api/plugin/cpp/PrintingPlanViews.hpp"
#include "libslic3r/Api/plugin/cpp/SeamPlacerViews.hpp"
#include "libslic3r/Api/plugin/cpp/SkirtBrimStepViews.hpp"
#include "libslic3r/Api/plugin/cpp/VolumeViews.hpp"

namespace slic3r_api {

// Read-only C++ view over one c_layer_config_range entry.
// The underlying data is borrowed from run_ctx_layer_height_generation and is
// only valid during the current setup_run()/run() call.
class LayerConfigRange
{
public:
    explicit LayerConfigRange(const c_layer_config_range *range) : m_range(range) { assert(m_range != nullptr); }

    coord_t z_min() const { return m_range->z_min; }
    coord_t z_max() const { return m_range->z_max; }
    Config config() const { return Config(m_range->config); }

private:
    const c_layer_config_range *m_range = nullptr;
};

class LayerConfigRanges
{
public:
    class iterator
    {
    public:
        explicit iterator(const c_layer_config_range *range) : m_range(range) {}

        LayerConfigRange operator*() const { return LayerConfigRange(m_range); }
        iterator &operator++() {
            ++m_range;
            return *this;
        }
        bool operator!=(const iterator &other) const { return m_range != other.m_range; }

    private:
        const c_layer_config_range *m_range = nullptr;
    };

    explicit LayerConfigRanges(const run_ctx_layer_height_generation &ctx)
        : m_ranges(ctx.layer_config_ranges), m_size(ctx.layer_config_ranges_size) {}

    uint32_t size() const { return m_size; }
    bool empty() const { return m_size == 0; }
    LayerConfigRange at(uint32_t idx) const {
        assert(idx < m_size);
        assert(m_ranges != nullptr);
        return LayerConfigRange(m_ranges + idx);
    }
    LayerConfigRange operator[](uint32_t idx) const {
        assert(idx < m_size);
        assert(m_ranges != nullptr);
        return LayerConfigRange(m_ranges + idx);
    }
    iterator begin() const { return iterator(m_ranges); }
    iterator end() const { return iterator(m_ranges != nullptr ? m_ranges + m_size : nullptr); }

private:
    const c_layer_config_range *m_ranges = nullptr;
    uint32_t m_size = 0;
};

} // namespace slic3r_api


#endif // slic3r_Api_plugin_cpp_Views_hpp_
