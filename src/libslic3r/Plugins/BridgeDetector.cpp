///|/ Copyright (c) SuperSlicer 2026 Durand Rémi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/

#include "BridgeDetector.hpp"

#include <algorithm>
#include <memory>
#include <vector>

#include "libslic3r/Api/plugin/c/slic3r_orchestrator.h"
#include "libslic3r/Api/plugin/c/steps/slic3r_step_bridge_detector.h"
#include "libslic3r/BridgeDetector.hpp"
#include "libslic3r/MultiPoint.hpp"
#include "libslic3r/Polygon.hpp"
#include "libslic3r/Polyline.hpp"

/*
BridgeDetector
==============

This plugin exposes the native bridge detector as a service for other
plugins. It does not participate in a slicing step that edits the print. A
caller provides one or more polygons, the lower-layer slices, and detector
parameters; the plugin returns an opaque detector instance through the C API.

The normal execution flow is:

    BridgeDetector::run()
    |-- validate the bridge-detector context
    |-- copy the input polygons and lower-layer slices
    |-- construct the native Slic3r::BridgeDetector
    `-- publish the detector context and its bridge_detector_vtable

    bridge_detector_vtable callbacks
    |-- detect_angle() computes the preferred bridge direction
    |-- coverage() returns polygons covered by bridge extrusion at an angle
    |-- unsupported_edges() returns unsupported boundary segments
    `-- getters and setters expose the detector state used by callers

The native state owns copies of all input geometry, because the ABI handles
passed to `run()` are borrowed. The returned vectors are copied into the
caller-provided storage and remain valid independently of temporary native
results. Invalid input or a failed native construction leaves no detector
instance to use. The ordinary plugin lifecycle methods are intentionally
no-ops because this class is a service provider rather than a data-processing
plugin.
*/

namespace slic3r_api { namespace BridgeDetectorPlugin {

namespace {

const char *k_bridge_detector_id = "bridge_detector.default";
const char *k_no_dependencies[] = { nullptr };

static Slic3r::ExPolygon *to_expolygon(expolygon_handle *me) { return reinterpret_cast<Slic3r::ExPolygon *>(me); }
static const Slic3r::ExPolygon *to_expolygon(const expolygon_handle *me) {
    return reinterpret_cast<const Slic3r::ExPolygon *>(me);
}
static const Slic3r::ExPolygons *to_expolygons(const expolygon_collection_handle *me) {
    return reinterpret_cast<const Slic3r::ExPolygons *>(me);
}
static Slic3r::Polygons *to_polygons(polygon_collection_handle *me) {
    return reinterpret_cast<Slic3r::Polygons *>(me);
}
static Slic3r::Polylines *to_polylines(polyline_collection_handle *me) {
    return reinterpret_cast<Slic3r::Polylines *>(me);
}

// Owns all native data needed by one bridge detector instance. Input geometry is
// copied because detector instances can outlive the borrowed ABI handles passed
// during creation.
struct NativeBridgeDetector
{
    Slic3r::ExPolygons expolygons_owned;
    Slic3r::ExPolygons lower_slices_owned;
    std::unique_ptr<Slic3r::BridgeDetector> detector;

    explicit NativeBridgeDetector(const bridge_detector_create_input &input)
    {
        if (input.lower_slices != nullptr)
            lower_slices_owned = *to_expolygons(input.lower_slices);

        if (input.expolygon != nullptr) {
            detector = std::make_unique<Slic3r::BridgeDetector>(*to_expolygon(input.expolygon),
                                                                lower_slices_owned,
                                                                input.spacing,
                                                                input.precision,
                                                                input.layer_id);
        } else if (input.expolygons != nullptr) {
            expolygons_owned = *to_expolygons(input.expolygons);
            detector = std::make_unique<Slic3r::BridgeDetector>(expolygons_owned,
                                                                lower_slices_owned,
                                                                input.spacing,
                                                                input.precision,
                                                                input.layer_id);
        }
    }
};

static void destroy(void *detector_ctx)
{
    // Called through bridge_detector_vtable by the C++ view/ABI consumer.
    delete static_cast<NativeBridgeDetector *>(detector_ctx);
}

static int32_t detect_angle(void *detector_ctx, double bridge_direction_override)
{
    NativeBridgeDetector *native = static_cast<NativeBridgeDetector *>(detector_ctx);
    return native != nullptr && native->detector != nullptr ?
        native->detector->detect_angle(bridge_direction_override) :
        0;
}

static uint32_t coverage(void *detector_ctx, double angle, polygon_collection_handle *out_polygons)
{
    // ABI collections are caller-owned output buffers. Always clear them first
    // so repeated calls return exactly the detector result for this angle.
    NativeBridgeDetector *native = static_cast<NativeBridgeDetector *>(detector_ctx);
    if (native == nullptr || native->detector == nullptr || out_polygons == nullptr)
        return 0;

    const Slic3r::Polygons polygons = native->detector->coverage(angle);
    Slic3r::Polygons &out = *to_polygons(out_polygons);
    out.clear();
    out.reserve(polygons.size());
    for (const Slic3r::Polygon &polygon : polygons)
        out.emplace_back(polygon);
    return static_cast<uint32_t>(out.size());
}

static uint32_t unsupported_edges(void *detector_ctx, double angle, polyline_collection_handle *out_polylines)
{
    // Same ownership rule as coverage(): the detector computes a native vector,
    // then copies it into the caller-provided ABI collection.
    NativeBridgeDetector *native = static_cast<NativeBridgeDetector *>(detector_ctx);
    if (native == nullptr || native->detector == nullptr || out_polylines == nullptr)
        return 0;

    const Slic3r::Polylines polylines = native->detector->unsupported_edges(angle);
    Slic3r::Polylines &out = *to_polylines(out_polylines);
    out.clear();
    out.reserve(polylines.size());
    for (const Slic3r::Polyline &polyline : polylines)
        out.emplace_back(polyline);
    return static_cast<uint32_t>(out.size());
}

static double get_angle(void *detector_ctx)
{
    NativeBridgeDetector *native = static_cast<NativeBridgeDetector *>(detector_ctx);
    return native != nullptr && native->detector != nullptr ? native->detector->angle : -1.;
}

static void set_max_bridge_length(void *detector_ctx, double max_bridge_length)
{
    NativeBridgeDetector *native = static_cast<NativeBridgeDetector *>(detector_ctx);
    if (native != nullptr && native->detector != nullptr)
        native->detector->max_bridge_length = max_bridge_length;
}

static double get_max_bridge_length(void *detector_ctx)
{
    NativeBridgeDetector *native = static_cast<NativeBridgeDetector *>(detector_ctx);
    return native != nullptr && native->detector != nullptr ? native->detector->max_bridge_length : -1.;
}

static void set_layer_id(void *detector_ctx, int32_t layer_id)
{
    NativeBridgeDetector *native = static_cast<NativeBridgeDetector *>(detector_ctx);
    if (native != nullptr && native->detector != nullptr)
        native->detector->layer_id = layer_id;
}

static int32_t get_layer_id(void *detector_ctx)
{
    NativeBridgeDetector *native = static_cast<NativeBridgeDetector *>(detector_ctx);
    return native != nullptr && native->detector != nullptr ? native->detector->layer_id : -1;
}

const bridge_detector_vtable native_bridge_detector_vtable = {
    // Function table consumed by bridge_detector_instance. Keep this order in
    // sync with slic3r_plugin_types.h.
    &destroy,
    &detect_angle,
    &coverage,
    &unsupported_edges,
    &get_angle,
    &set_max_bridge_length,
    &get_max_bridge_length,
    &set_layer_id,
    &get_layer_id
};

plugin_vtable default_plugin_vtable = {
    // Plugin registration vtable. This plugin is a service provider: run()
    // creates a detector instance instead of editing print data directly.
    SLIC3R_PLUGIN_ABI_VERSION,
    &BridgeDetector::get_id_bridge,
    &BridgeDetector::get_name_bridge,
    &BridgeDetector::get_description_bridge,
    &BridgeDetector::get_exclusive_group_bridge,
    &BridgeDetector::get_exclusive_group_label_bridge,
    &BridgeDetector::get_exclusive_group_tooltip_bridge,
    &BridgeDetector::get_step_bridge,
    &BridgeDetector::get_dependencies_bridge,
    &BridgeDetector::get_priority_bridge,
    &BridgeDetector::used_config_keys_bridge,
    &BridgeDetector::defined_config_keys_bridge,
    &BridgeDetector::initialize_bridge,
    &BridgeDetector::setup_bridge,
    &BridgeDetector::setup_run_bridge,
    &BridgeDetector::run_bridge
};

} // namespace

void BridgeDetector::initialize(storage_handle * storage) const {}

void BridgeDetector::setup(const plugin_run_context *, uint32_t) const {}

void BridgeDetector::setup_run(const plugin_run_context *) const {}

void BridgeDetector::run(const plugin_run_context *run_ctx) const
{
    // BRIDGE_DETECTOR contexts contain a creation input and an empty output
    // instance. On success, ownership of NativeBridgeDetector is transferred to
    // ctx->detector and later released through native_bridge_detector_vtable.
    run_ctx_bridge_detector *ctx = plugin_ctx_as_bridge_detector(run_ctx);
    if (ctx == nullptr)
        return;

    std::unique_ptr<NativeBridgeDetector> detector = std::make_unique<NativeBridgeDetector>(ctx->input);
    if (detector->detector == nullptr)
        return;

    ctx->detector.ctx = detector.release();
    ctx->detector.vt = &native_bridge_detector_vtable;
}

BridgeDetector &BridgeDetector::instance(orchestrator_handle *orch)
{
    static BridgeDetector s_instance(orch);
    return s_instance;
}

plugin_instance BridgeDetector::c_instance() const
{
    // Expose this C++ singleton as a plain C plugin_instance. The host stores the
    // opaque ctx pointer and calls back through default_plugin_vtable.
    plugin_instance instance = {};
    instance.ctx = const_cast<BridgeDetector *>(this);
    instance.vt = &default_plugin_vtable;
    return instance;
}

const char *BridgeDetector::id() const noexcept
{
    return k_bridge_detector_id;
}

const char *BridgeDetector::name() const noexcept
{
    return "Bridge detector";
}

const char *BridgeDetector::description() const noexcept
{
    return "Detect bridge direction and unsupported bridge edges for other plugins.";
}

const char *BridgeDetector::exclusive_group() const noexcept
{
    return "";
}

const char *BridgeDetector::exclusive_group_label() const noexcept
{
    return "";
}

const char *BridgeDetector::exclusive_group_tooltip() const noexcept
{
    return "";
}

slicing_step_t BridgeDetector::step() const noexcept
{
    return BRIDGE_DETECTOR;
}

const char *const *BridgeDetector::dependencies() const noexcept
{
    return k_no_dependencies;
}

int32_t BridgeDetector::priority() const noexcept
{
    return -100000;
}

const char *BridgeDetector::get_id_bridge(void *plugin_ctx)
{
    return static_cast<BridgeDetector *>(plugin_ctx)->id();
}

const char *BridgeDetector::get_name_bridge(void *plugin_ctx)
{
    return static_cast<BridgeDetector *>(plugin_ctx)->name();
}

const char *BridgeDetector::get_description_bridge(void *plugin_ctx)
{
    return static_cast<BridgeDetector *>(plugin_ctx)->description();
}

const char *BridgeDetector::get_exclusive_group_bridge(void *plugin_ctx)
{
    return static_cast<BridgeDetector *>(plugin_ctx)->exclusive_group();
}

const char *BridgeDetector::get_exclusive_group_label_bridge(void *plugin_ctx)
{
    return static_cast<BridgeDetector *>(plugin_ctx)->exclusive_group_label();
}

const char *BridgeDetector::get_exclusive_group_tooltip_bridge(void *plugin_ctx)
{
    return static_cast<BridgeDetector *>(plugin_ctx)->exclusive_group_tooltip();
}

slicing_step_t BridgeDetector::get_step_bridge(void *plugin_ctx)
{
    return static_cast<BridgeDetector *>(plugin_ctx)->step();
}

const_strings_t BridgeDetector::get_dependencies_bridge(void *plugin_ctx)
{
    const_strings_t out = {};
    const char *const *deps = static_cast<BridgeDetector *>(plugin_ctx)->dependencies();
    uint32_t count = 0;
    while (deps != nullptr && deps[count] != nullptr)
        ++count;
    out.items = deps;
    out.size = count;
    return out;
}

int32_t BridgeDetector::get_priority_bridge(void *plugin_ctx)
{
    return static_cast<BridgeDetector *>(plugin_ctx)->priority();
}

int32_t BridgeDetector::used_config_keys_bridge(void *, raw_used_config_key *)
{
    return 0;
}

int32_t BridgeDetector::defined_config_keys_bridge(void *, const char **)
{
    return 0;
}

void BridgeDetector::initialize_bridge(void *plugin_ctx, storage_handle *storage)
{
    static_cast<BridgeDetector *>(plugin_ctx)->initialize(storage);
}

void BridgeDetector::setup_bridge(void *plugin_ctx, const plugin_run_context *run_ctx, uint32_t run_count)
{
    static_cast<BridgeDetector *>(plugin_ctx)->setup(run_ctx, run_count);
}

void BridgeDetector::setup_run_bridge(void *plugin_ctx, const plugin_run_context *run_ctx)
{
    static_cast<BridgeDetector *>(plugin_ctx)->setup_run(run_ctx);
}

void BridgeDetector::run_bridge(void *plugin_ctx, const plugin_run_context *run_ctx)
{
    static_cast<BridgeDetector *>(plugin_ctx)->run(run_ctx);
}

void register_bridge_detector_plugin(orchestrator_handle *orch)
{
    orchestrator_register_plugin(orch, BridgeDetector::instance(orch).c_instance());
}

}} // namespace slic3r_api::BridgeDetectorPlugin

#ifdef BRIDGE_DETECTOR_PLUGIN_DLL
extern "C" void register_plugin(orchestrator_handle *orch)
{
    slic3r_api::BridgeDetectorPlugin::register_bridge_detector_plugin(orch);
}
#endif // BRIDGE_DETECTOR_PLUGIN_DLL
