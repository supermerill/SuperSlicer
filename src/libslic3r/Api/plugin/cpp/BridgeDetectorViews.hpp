///|/ Copyright (c) SuperSlicer 2026 Durand Rémi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/
#ifndef slic3r_Api_plugin_cpp_BridgeDetectorViews_hpp_
#define slic3r_Api_plugin_cpp_BridgeDetectorViews_hpp_

#include "libslic3r/Api/plugin/c/slic3r_bridge_detector.h"
#include "libslic3r/Api/plugin/cpp/GeometryViews.hpp"

namespace slic3r_api {

/*
Bridge detector C++ API
=======================

`BridgeDetector` is the C++ RAII wrapper for the bridge-detector service
exposed through the C plugin ABI. It owns the returned
`bridge_detector_instance`, but it does not own the source polygons passed to
the service. The built-in provider copies the geometry it needs when the
instance is created.

The usual consumer-side sequence is:

    create bridge_detector_create_input
        |-- set expolygon or expolygons
        |-- set lower_slices, spacing, precision, and layer_id
        `-- call orchestrator_create_bridge_detector()
    construct BridgeDetector from the returned instance
        |-- check valid()
        |-- optionally set max_bridge_length
        |-- call detect_angle()
        |-- read angle()
        `-- request coverage() or unsupported_edges()
    let the BridgeDetector destructor release the service instance

An input normally contains either one `expolygon` or an `expolygons` collection.
If both are supplied, the provider gives priority to `expolygon`. `spacing`
and `precision` are expressed in the scaled coordinate units used by the
geometry ABI; `layer_id` identifies the layer being analyzed. Passing `-1.`
as an angle asks the provider to use its automatically detected direction.
`max_bridge_length` is also expressed in the scaled length units expected by
the ABI.

Typical bridge coverage query:

    bridge_detector_create_input input = {};
    input.expolygon = unsupported.handle();
    input.lower_slices = lower_layer.slices().handle();
    input.spacing = bridge_flow.spacing;
    input.precision = scale_i(bridge_precision_mm);
    input.layer_id = layer_idx;

    BridgeDetector detector(
        orchestrator_create_bridge_detector(orchestrator, &input));

    if (!detector.valid())
        return;

    detector.set_max_bridge_length(scale_d(max_bridge_length_mm));

    if (!detector.detect_angle())
        return;

    StoredPolygonCollection coverage(storage);
    if (detector.coverage(coverage) > 0) {
        // Use the bridge-covered polygons.
    }

`coverage()` writes into the caller-provided mutable collection, so a
`StoredPolygonCollection` is used when the result must be retained. To obtain
unsupported boundary segments instead, use a polyline collection:

    StoredPolylineCollection unsupported_edges(storage);
    if (detector.detect_angle())
        detector.unsupported_edges(unsupported_edges);

`coverage()` describes areas that can be covered by bridge extrusion, whereas
`unsupported_edges()` describes unsupported boundary segments. Both methods
return the number of generated output items.

The wrapper is deliberately move-only:

    BridgeDetector first = create_detector();
    BridgeDetector second = std::move(first);

Moving transfers the only destruction responsibility and leaves `first`
invalid but safe to destroy or reset. Copying is disabled because two wrappers
must never destroy the same ABI instance. An invalid instance makes queries
return `false`, `0`, or `-1.` as appropriate, and makes setters no-ops.

This class is intentionally separate from `DataTreeViews.hpp`: it is not a
view over the slicer data tree, and it does not document the mathematical
bridge-detection algorithm. It documents only the service contract and the
lifetime rules needed by plugin consumers.
*/

class BridgeDetector
{
public:
    // Creates an invalid wrapper. A valid service instance must be supplied by
    // the constructor taking bridge_detector_instance.
    BridgeDetector() = default;

    // Takes ownership of the ABI instance and destroys it on reset or at scope
    // exit. The input geometry remains owned by the caller/provider contract.
    explicit BridgeDetector(bridge_detector_instance instance) : m_instance(instance) {}

    // Copying is forbidden because the ABI instance has one owner.
    BridgeDetector(const BridgeDetector &) = delete;
    BridgeDetector &operator=(const BridgeDetector &) = delete;

    // Moving transfers ownership and clears the source wrapper.
    BridgeDetector(BridgeDetector &&other) noexcept : m_instance(other.m_instance) { other.m_instance = {}; }
    BridgeDetector &operator=(BridgeDetector &&other) noexcept {
        if (this != &other) {
            reset();
            m_instance = other.m_instance;
            other.m_instance = {};
        }
        return *this;
    }

    // Releases the owned ABI instance, if any.
    ~BridgeDetector() { reset(); }

    // Returns whether the ABI handle and its callback table are both usable.
    bool valid() const { return m_instance.ctx != nullptr && m_instance.vt != nullptr; }
    explicit operator bool() const { return valid(); }

    // Detects the bridge direction, or uses the supplied direction when it is
    // not the automatic-angle sentinel (-1.).
    bool detect_angle(double bridge_direction_override = -1.) {
        return bridge_detector_detect_angle(&m_instance, bridge_direction_override) != 0;
    }

    // Appends bridge-covered polygons to out_polygons and returns their count.
    uint32_t coverage(StoredPolygonCollection &out_polygons, double angle = -1.) {
        return bridge_detector_coverage(&m_instance, angle, out_polygons.mutable_handle());
    }

    // Appends unsupported boundary polylines to out_polylines and returns their
    // count.
    uint32_t unsupported_edges(StoredPolylineCollection &out_polylines, double angle = -1.) {
        return bridge_detector_unsupported_edges(&m_instance, angle, out_polylines.mutable_handle());
    }

    // Returns the currently selected bridge direction, or -1. for an invalid
    // or not-yet-available service result.
    double angle() { return bridge_detector_get_angle(&m_instance); }

    // Sets the maximum bridge length in the scaled ABI length units.
    void set_max_bridge_length(double max_bridge_length) {
        bridge_detector_set_max_bridge_length(&m_instance, max_bridge_length);
    }

    // Returns the configured maximum bridge length, or -1. when unavailable.
    double max_bridge_length() { return bridge_detector_get_max_bridge_length(&m_instance); }

    // Changes the layer identifier associated with subsequent service queries.
    void set_layer_id(int32_t layer_id) { bridge_detector_set_layer_id(&m_instance, layer_id); }

    // Returns the layer identifier stored by the service, or -1. when invalid.
    int32_t layer_id() { return bridge_detector_get_layer_id(&m_instance); }

    // Destroys the service instance and leaves this wrapper invalid. Repeated
    // calls are safe because the ABI handle is cleared after destruction.
    void reset() {
        bridge_detector_destroy(&m_instance);
        m_instance = {};
    }

private:
    bridge_detector_instance m_instance = {};
};

} // namespace slic3r_api


#endif // slic3r_Api_plugin_cpp_BridgeDetectorViews_hpp_
