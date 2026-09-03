///|/ Copyright (c) SuperSlicer 2026 Durand Rémi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/

#include "StandardLayerHeightGenerator.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "libslic3r/Api/plugin/c/slic3r_orchestrator.h"
#include "libslic3r/Api/plugin/c/steps/slic3r_step_layer_height.h"
#include "libslic3r/Api/plugin/cpp/PrintHelpers.hpp"
#include "libslic3r/Api/plugin/cpp/Views.hpp"

#ifdef _DEBUG
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintObject.hpp"
#include "libslic3r/Slicing.hpp"
#endif

/*
Standard layer-height generation
================================

This plugin provides the default implementation of STEP_LAYER_HEIGHT. The
host runs it once for each printable Object. It does not create Layer objects
itself; it computes the object-local layer profile and publishes it through
the step context so STEP_SLICING can create the layers afterwards.

The normal configuration path reconstructs the small subset of native slicing
parameters needed by the profile algorithm. It combines the object's layer
configuration ranges, clamps them to the object height and Z step, and fills
gaps with the default layer height. The resulting compact profile is stored as
`[z, height, z, height, ...]`. It is then expanded into the explicit layer
descriptors consumed by the slicing step, interpolating between profile points
when necessary.

Support and model extruders contribute nozzle-based minimum and maximum layer
height limits. A zero minimum or maximum uses the corresponding nozzle-based
fallback, and all values are aligned to `z_step`. An enforced list of layer Z
positions supplied by the host has priority over configuration ranges and is
forwarded directly to the context callback.

The normal call flow is:

    register_standard_layer_height_generator_plugin()
    `-- StandardLayerHeightGenerator::instance()
        `-- orchestrator_register_plugin()
            `-- StandardLayerHeightGenerator::run_impl()
                |-- read the layer-height context for one Object
                |-- enforced Z positions?
                |   `-- set_layer_height_profile() with the host-provided list
                `-- otherwise
                    |-- make_layer_height_slicing_parameters()
                    |   `-- resolve nozzle, support, and object limits
                    |-- layer_height_profile_from_ranges()
                    |   `-- trim, fill, and compact configured ranges
                    |-- layer_descriptors_from_height_profile()
                    |   `-- interpolate and emit explicit [z, height] pairs
                    `-- set_layer_height_profile() for STEP_SLICING

The profile is intentionally object-local. Raft layers are handled outside
this object layer plan, so they do not alter the first object layer height or
the profile's Z reference.
*/

namespace slic3r_api { namespace StandardLayerHeightGeneratorPlugin {

namespace {

const char *k_standard_layer_height_generator_id = "standard_layer_height_generator";
const char *k_no_dependencies[] = {nullptr};

} // namespace

using LayerHeightRange = std::pair<coord_t, coord_t>;

#define MIN_LAYER_HEIGHT 0.005

struct LayerHeightSlicingParameters
{
    // Minimal subset of SlicingParameters needed to reproduce the native layer
    // height profile generation through the public plugin views.
    bool first_object_layer_height_fixed = false;
    coord_t first_object_layer_height = 0;
    coord_t object_print_z_height = 0;
    coord_t z_step = 0;
    coord_t layer_height = 0;
    coord_t min_layer_height = 0;
    coord_t max_layer_height = 0;
};

bool append_mismatch(std::ostringstream &out, const char *name, coord_t calculated, coord_t native)
{
    // Keep debug comparison output compact: append only fields that differ.
    if (calculated == native)
        return false;
    out << name << ": calculated=" << calculated << ", native=" << native << '\n';
    return true;
}

bool append_mismatch(std::ostringstream &out, const char *name, bool calculated, bool native)
{
    if (calculated == native)
        return false;
    out << name << ": calculated=" << (calculated ? "true" : "false")
        << ", native=" << (native ? "true" : "false") << '\n';
    return true;
}

#ifdef _DEBUG
bool test_layer_height_slicing_parameters(const LayerHeightSlicingParameters &calculated,
                                          const LayerHeightSlicingParameters &native,
                                          std::string &out_error)
{
    // This does not test the ABI itself. It verifies that the plugin-side
    // reconstruction stays bit-identical to the host-native SlicingParameters
    // values while the migration is in progress.
    std::ostringstream msg;
    bool mismatch = false;
    mismatch |= append_mismatch(msg, "first_object_layer_height_fixed",
                                calculated.first_object_layer_height_fixed,
                                native.first_object_layer_height_fixed);
    mismatch |= append_mismatch(msg, "first_object_layer_height",
                                calculated.first_object_layer_height,
                                native.first_object_layer_height);
    mismatch |= append_mismatch(msg, "object_print_z_height",
                                calculated.object_print_z_height,
                                native.object_print_z_height);
    mismatch |= append_mismatch(msg, "z_step", calculated.z_step, native.z_step);
    mismatch |= append_mismatch(msg, "layer_height", calculated.layer_height, native.layer_height);

    if (mismatch)
        out_error = msg.str();
    return !mismatch;
}

LayerHeightSlicingParameters layer_height_slicing_parameters_from_native(const Slic3r::PrintObject &object)
{
    // Host-only adapter for debug tests. It is intentionally excluded from
    // release/plugin ABI builds.
    const Slic3r::SlicingParameters &native = object.slicing_parameters();
    LayerHeightSlicingParameters out;
    out.first_object_layer_height_fixed = native.first_object_layer_height_fixed();
    out.first_object_layer_height = scale_i(native.first_object_layer_height);
    out.object_print_z_height = scale_i(native.object_print_z_height());
    out.z_step = scale_i(native.z_step);
    out.layer_height = scale_i(native.layer_height);
    return out;
}
#endif

coord_t min_layer_height_from_nozzle(const Config &print_config, uint32_t extruder_idx)
{
    // Mirror the native fallback: a disabled/zero minimum means half the nozzle
    // diameter, clamped to a tiny hard lower bound and aligned on z_step.
    const ConfigOption nozzle_diameter_opt = print_config.get("nozzle_diameter");
    const uint32_t nozzle_idx = extruder_idx < nozzle_diameter_opt.size() ? extruder_idx : 0;
    const double nozzle_diameter = nozzle_diameter_opt.get_float(nozzle_idx);
    const ConfigOption min_layer_height_opt = print_config.get("min_layer_height");
    assert(extruder_idx < min_layer_height_opt.size());
    double value = min_layer_height_opt.get_effective_value(nozzle_diameter, extruder_idx);
    if (value == 0.0)
        value = nozzle_diameter / 2;
    return check_z_step(scale_i(std::max(MIN_LAYER_HEIGHT, value)), scale_i(print_config.get("z_step").get_float()));
}

coord_t max_layer_height_from_nozzle(const Config &print_config, uint32_t extruder_idx)
{
    // Mirror the native fallback: a disabled/zero maximum means 75% of nozzle
    // diameter, but never lower than the effective minimum layer height.
    const ConfigOption nozzle_diameter_opt = print_config.get("nozzle_diameter");
    const uint32_t nozzle_idx = extruder_idx < nozzle_diameter_opt.size() ? extruder_idx : 0;
    const double nozzle_diameter = nozzle_diameter_opt.get_float(nozzle_idx);
    const ConfigOption max_layer_height_opt = print_config.get("max_layer_height");
    assert(extruder_idx < max_layer_height_opt.size());
    double value = max_layer_height_opt.get_effective_value(nozzle_diameter, nozzle_idx);
    if (value == 0.0 || !max_layer_height_opt.is_enabled(nozzle_idx))
        value = 0.75 * nozzle_diameter;
    const coord_t min_layer_height = min_layer_height_from_nozzle(print_config, extruder_idx);
    return check_z_step(std::max(min_layer_height, scale_i(value)), scale_i(print_config.get("z_step").get_float()));
}

uint32_t config_extruder_idx(int extruder_id)
{
    // Config values are 1-based for selected extruders, while vector-style
    // ConfigOption access is 0-based. Non-positive means "default extruder".
    return extruder_id > 0 ? uint32_t(extruder_id - 1) : 0;
}

LayerHeightSlicingParameters make_layer_height_slicing_parameters(Print print, Object object) {
    // Recreate only the fields required for layer height profile generation.
    // Everything here must stay expressible through public plugin views.
    const Config print_config = print.config();
    const Config object_config = object.config();
    const std::set<uint16_t> extruders = object_extruders(print, object);

    LayerHeightSlicingParameters out;
    out.z_step = scale_i(print_config.get("z_step").get_float());
    out.layer_height = check_z_step(scale_i(object_config.get("layer_height").get_float()), out.z_step);

    const ConfigOption nozzle_diameter_opt = print_config.get("nozzle_diameter");
    const ConfigOption first_layer_height_opt = object_config.get("first_layer_height");
    coord_t first_layer_height = 0;
    if (first_layer_height_opt.is_percent()) {
        // Percent first-layer height is relative to the nozzles used by this
        // object. Pick the smallest effective value, matching native behavior.
        first_layer_height = std::numeric_limits<coord_t>::max();
        for (uint16_t extruder_idx : extruders) {
            if (extruder_idx >= nozzle_diameter_opt.size())
                continue;
            const double nozzle = nozzle_diameter_opt.get_float(extruder_idx);
            first_layer_height = std::min(first_layer_height, scale_i(first_layer_height_opt.get_effective_value(nozzle)));
        }
        if (first_layer_height == std::numeric_limits<coord_t>::max())
            first_layer_height = 0;
    } else {
        first_layer_height = scale_i(first_layer_height_opt.get_float());
    }
    first_layer_height = check_z_step(first_layer_height, out.z_step);
    if (first_layer_height <= SCALED_EPSILON)
        first_layer_height = out.layer_height;

    out.first_object_layer_height = first_layer_height;
    // object.max_z can also be taken from this step context, but keeping it on
    // the Object view makes the plugin code independent from host internals.
    out.object_print_z_height = check_z_step(object.max_z(), out.z_step);
    if (out.object_print_z_height + SCALED_EPSILON < object.max_z())
        out.object_print_z_height += out.z_step;

    coord_t min_layer_height = 0;
    coord_t max_layer_height = std::numeric_limits<coord_t>::max();

    const bool has_support = object_config.get("support_material").get_bool() ||
        object_config.get("support_material_enforce_layers").get_int() > 0;
    if (has_support) {
        // Support extruders constrain the layer height even if they are not part
        // of the object's printable model extruder set. Raft alone is excluded:
        // it is generated outside the object layer plan.
        const int support_extruder = object_config.get("support_material_extruder").get_int();
        const int support_interface_extruder = object_config.get("support_material_interface_extruder").get_int();
        if (support_extruder > 0) {
            const uint32_t idx = config_extruder_idx(support_extruder);
            min_layer_height = std::max(min_layer_height, min_layer_height_from_nozzle(print_config, idx));
            max_layer_height = std::min(max_layer_height, max_layer_height_from_nozzle(print_config, idx));
        }
        if (support_interface_extruder > 0) {
            const uint32_t idx = config_extruder_idx(support_interface_extruder);
            min_layer_height = std::max(min_layer_height, min_layer_height_from_nozzle(print_config, idx));
            max_layer_height = std::min(max_layer_height, max_layer_height_from_nozzle(print_config, idx));
        }
    }

    if (extruders.empty()) {
        // Empty object extruder set still needs a sane default range, otherwise
        // layer_height could stay unclamped.
        min_layer_height = std::max(min_layer_height, min_layer_height_from_nozzle(print_config, 0));
        max_layer_height = std::min(max_layer_height, max_layer_height_from_nozzle(print_config, 0));
    } else {
        for (uint16_t extruder_idx : extruders) {
            min_layer_height = std::max(min_layer_height, min_layer_height_from_nozzle(print_config, extruder_idx));
            max_layer_height = std::min(max_layer_height, max_layer_height_from_nozzle(print_config, extruder_idx));
        }
    }

    if (max_layer_height == std::numeric_limits<coord_t>::max())
        max_layer_height = out.layer_height;
    if (min_layer_height == 0)
        min_layer_height = out.layer_height;

    out.min_layer_height = check_z_step(min_layer_height, out.z_step);
    out.max_layer_height = check_z_step(max_layer_height, out.z_step);
    if (out.max_layer_height < out.min_layer_height)
        out.max_layer_height = out.min_layer_height;
    out.layer_height = std::clamp(out.layer_height, out.min_layer_height, out.max_layer_height);

    // STEP_LAYER_HEIGHT emits object-local layers. Raft generation must not
    // change the object first-layer height or whether it is fixed.
    out.first_object_layer_height_fixed = true;

    return out;
}

std::vector<coord_t> layer_descriptors_from_height_profile(const LayerHeightSlicingParameters &slicing_params,
                                                           const std::vector<coord_t> &layer_height_profile)
{
    // Expand the compact [z, height] profile produced by this plugin into the
    // explicit [layer_top_z, layer_height] pairs consumed by STEP_SLICING. This
    // keeps the old profile interpolation local to the default layer-height
    // generator instead of letting slicing re-run the profile expansion.
    std::vector<coord_t> out;
    if (layer_height_profile.empty())
        return out;

    coord_t print_z = 0;
    if (slicing_params.first_object_layer_height_fixed) {
        print_z = slicing_params.first_object_layer_height;
        out.push_back(print_z);
        out.push_back(slicing_params.first_object_layer_height);
    }

    size_t idx_layer_height_profile = 0;
    coord_t slice_z = print_z + slicing_params.min_layer_height / 2;
    while (slice_z < slicing_params.object_print_z_height) {
        coord_t height = slicing_params.min_layer_height;
        if (idx_layer_height_profile < layer_height_profile.size()) {
            size_t next = idx_layer_height_profile + 2;
            for (;;) {
                if (next >= layer_height_profile.size() || slice_z < layer_height_profile[next])
                    break;
                idx_layer_height_profile = next;
                next += 2;
            }

            height = layer_height_profile[idx_layer_height_profile + 1];
            if (next < layer_height_profile.size()) {
                const coord_t z1 = layer_height_profile[idx_layer_height_profile];
                const coord_t h1 = layer_height_profile[idx_layer_height_profile + 1];
                const coord_t z2 = layer_height_profile[next];
                const coord_t h2 = layer_height_profile[next + 1];
                const double t = z2 == z1 ? 0.0 : double(slice_z - z1) / double(z2 - z1);
                height = coord_t(std::llround(double(h1) + (double(h2) - double(h1)) * t));
            }
            height = check_z_step(height, slicing_params.z_step);
        }

        slice_z = print_z + height / 2;
        if (slice_z >= slicing_params.object_print_z_height)
            break;

        print_z += height;
        out.push_back(print_z);
        out.push_back(height);
        slice_z = print_z + slicing_params.min_layer_height / 2;
    }

    return out;
}

// Convert layer_config_ranges to layer_height_profile. Both are referenced to z=0,
// meaning raft layers are not part of this profile; later G-code generation may
// lift the printed object by the raft thickness.
std::vector<coord_t> layer_height_profile_from_ranges(Print print,
                                                      Object object,
                                                      LayerConfigRanges layer_config_ranges) {
    LayerHeightSlicingParameters slicing_params = make_layer_height_slicing_parameters(print, object);

    // 1) Trim ranges to be non-overlapping. User/model ranges are sorted
    // lexicographically, so each new low bound can be clipped to the previous
    // accepted high bound.
    std::vector<std::pair<LayerHeightRange, coord_t>> ranges_non_overlapping;
    ranges_non_overlapping.reserve(layer_config_ranges.size() * 4);
    if (slicing_params.first_object_layer_height_fixed)
        ranges_non_overlapping.push_back(
            std::pair<LayerHeightRange, coord_t>(LayerHeightRange(0., slicing_params.first_object_layer_height),
                                                 slicing_params.first_object_layer_height));
    // The height ranges are sorted lexicographically by low / high layer boundaries.
    for (LayerConfigRange range : layer_config_ranges) {
        coord_t lo = range.z_min();
        coord_t hi = std::min(range.z_max(), slicing_params.object_print_z_height);
        coord_t height = scale_i(range.config().get("layer_height").get_float());
        if (!ranges_non_overlapping.empty())
            // Trim current low with the last high.
            lo = std::max(lo, ranges_non_overlapping.back().first.second);
        lo = check_z_step(lo, slicing_params.z_step);
        hi = check_z_step(hi, slicing_params.z_step);
        height = check_z_step(height, slicing_params.z_step);
        if (lo + EPSILON < hi)
            // Ignore too narrow ranges.
            ranges_non_overlapping.push_back(std::pair<LayerHeightRange, coord_t>(LayerHeightRange(lo, hi), height));
    }

    // 2) Convert the trimmed ranges to a compact step profile. Undefined
    // intervals between z=0 and object_print_z_height use the default layer
    // height.
    std::vector<coord_t> layer_height_profile;
    auto last_z = [&layer_height_profile]() {
        return layer_height_profile.size() < 2 ? coord_t(0) : *(layer_height_profile.end() - 2);
    };
    auto lh_append = [&layer_height_profile](coord_t z, coord_t layer_height) {
        // Profiles are stored as [z, height, z, height, ...]. Avoid duplicate
        // adjacent entries so the host receives a stable compact profile.
        if (layer_height_profile.size() > 1) {
            bool last_z_matches = (*(layer_height_profile.end() - 2) == z);
            bool last_h_matches = (layer_height_profile.back() == layer_height);
            if (last_h_matches) {
                if (last_z_matches) {
                    // Drop a duplicate.
                    return;
                }
                if (layer_height_profile.size() >= 4 && (*(layer_height_profile.end() - 3) == layer_height)) {
                    // Third repetition of the same layer_height. Update z of the last entry.
                    *(layer_height_profile.end() - 2) = z;
                    return;
                }
            }
        }
        layer_height_profile.push_back(z);
        layer_height_profile.push_back(layer_height);
    };

    for (const std::pair<LayerHeightRange, coord_t> &non_overlapping_range : ranges_non_overlapping) {
        coord_t lo = non_overlapping_range.first.first;
        coord_t hi = non_overlapping_range.first.second;
        coord_t height = non_overlapping_range.second;
        if (coord_t z = last_z(); lo > z + EPSILON) {
            // Insert a step of normal layer height.
            lh_append(z, slicing_params.layer_height);
            lh_append(lo, slicing_params.layer_height);
        }
        // Insert a step of the overriden layer height.
        lh_append(lo, height);
        lh_append(hi, height);
    }

    if (coord_t z = last_z(); z + EPSILON < slicing_params.object_print_z_height) {
        // Insert a step of normal layer height up to the object top.
        lh_append(z, slicing_params.layer_height);
        lh_append(slicing_params.object_print_z_height, slicing_params.layer_height);
    }

    return layer_height_profile;
}

void StandardLayerHeightGenerator::run_impl(const plugin_run_context *run_ctx) const {
    // STEP_LAYER_HEIGHT is object-local. The host passes one Object and a setter
    // callback; the plugin either forwards enforced Z values or computes the
    // normal profile from config ranges.
    if (plugin_ctx_as_layer_height_generation(run_ctx) == nullptr)
        return;
    const run_ctx_layer_height_generation &ctx = *plugin_ctx_as_layer_height_generation(run_ctx);
    if (ctx.print == nullptr || ctx.print == nullptr)
        return;

    Print print(ctx.print);
    Object object(ctx.object);
    storage_handle *storage = run_ctx->plugin_storage;
    (void)storage;

    if (ctx.enforce_layer_zs != nullptr && ctx.enforce_layer_zs_size > 0) {
        // Host-enforced layer Zs take precedence over config ranges.
        ctx.set_layer_height_profile(ctx.object, ctx.enforce_layer_zs, ctx.enforce_layer_zs_size);
    } else {
        LayerConfigRanges layer_config_ranges(ctx);
        std::vector<coord_t> layer_height_profile = layer_height_profile_from_ranges(print,object, layer_config_ranges);
        if (!layer_height_profile.empty()) {
            std::vector<coord_t> layer_descriptors = layer_descriptors_from_height_profile(
                make_layer_height_slicing_parameters(print, object),
                layer_height_profile);
            ctx.set_layer_height_profile(ctx.object, layer_descriptors.data(), layer_descriptors.size());
        } else {
            ctx.set_layer_height_profile(ctx.object, nullptr, 0);
        }
    }
}

StandardLayerHeightGenerator &StandardLayerHeightGenerator::instance(orchestrator_handle *orch) {
    static StandardLayerHeightGenerator s_instance(orch);
    return s_instance;
}

const char *StandardLayerHeightGenerator::id_impl() const noexcept { return k_standard_layer_height_generator_id; }

const char *StandardLayerHeightGenerator::name_impl() const noexcept { return "Standard layer height"; }

const char *StandardLayerHeightGenerator::description_impl() const noexcept
{
    return "Generate the usual object layer descriptors from object settings and enforced layer positions.";
}

slicing_step_t StandardLayerHeightGenerator::step_impl() const noexcept { return STEP_LAYER_HEIGHT; }

const char *const *StandardLayerHeightGenerator::dependencies_impl() const noexcept { return k_no_dependencies; }

int32_t StandardLayerHeightGenerator::priority_impl() const noexcept { return 0; }

#ifdef _DEBUG
bool test_layer_height_slicing_parameters(const Slic3r::Print &native_print,
                                          const Slic3r::PrintObject &native_object,
                                          std::string &out_error)
{
    // Wrap native host objects as read-only plugin views, then compare the
    // plugin-side reconstruction with the native cached parameters.
    Print print(reinterpret_cast<const print_handle *>(&native_print));
    Object object(reinterpret_cast<const object_handle *>(&native_object));
    return test_layer_height_slicing_parameters(make_layer_height_slicing_parameters(print, object),
                                                layer_height_slicing_parameters_from_native(native_object),
                                                out_error);
}
#endif

void register_standard_layer_height_generator_plugin(orchestrator_handle *orch) {
    orchestrator_register_plugin(orch, StandardLayerHeightGenerator::instance(orch).c_instance());
}

}} // namespace slic3r_api::StandardLayerHeightGeneratorPlugin

#ifdef STANDARD_LAYER_HEIGHT_GENERATOR_PLUGIN_DLL
extern "C" void register_plugin(orchestrator_handle *orch) {
    slic3r_api::StandardLayerHeightGeneratorPlugin::register_standard_layer_height_generator_plugin(orch);
}
#endif // STANDARD_LAYER_HEIGHT_GENERATOR_PLUGIN_DLL
