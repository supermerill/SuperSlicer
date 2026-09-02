///|/ Copyright (c) SuperSlicer 2026 Durand Rémi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/
#ifndef slic3r_Api_plugin_cpp_Views_hpp_
#define slic3r_Api_plugin_cpp_Views_hpp_

#include <cassert>

#include "libslic3r/Api/plugin/cpp/BridgeDetectorViews.hpp"
#include "libslic3r/Api/plugin/cpp/ClipperViews.hpp"
#include "libslic3r/Api/plugin/cpp/DataTreeViews.hpp"
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
