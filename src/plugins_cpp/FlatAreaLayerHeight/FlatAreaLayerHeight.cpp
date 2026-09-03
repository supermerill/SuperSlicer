///|/ Copyright (c) SuperSlicer 2026 Durand Rémi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/
///|/ AI - generated

#include "FlatAreaLayerHeight.hpp"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <limits>
#include <map>
#include <set>
#include <utility>
#include <vector>

#include "libslic3r/Api/plugin/c/slic3r_config_def.h"
#include "libslic3r/Api/plugin/c/slic3r_orchestrator.h"
#include "libslic3r/Api/plugin/c/steps/slic3r_step_layer_height.h"
#include "libslic3r/Api/plugin/cpp/Views.hpp"

namespace slic3r_api { namespace FlatAreaLayerHeightPlugin {

namespace {

const char *k_flat_area_layer_height_id = "flat_area_layer_height";
const char *k_no_dependencies[] = { nullptr };
const raw_used_config_key k_used_config_keys[] = {
    { "layer_height_min_flat_area", RAW_CO_FLOAT, RAW_CONTAINER_TYPE_PROJECT, RAW_PRESET_TYPE_FFF_PRINT }
};
const char *k_defined_config_keys[] = { "layer_height_min_flat_area" };

// Keep a physical lower bound even if printer settings allow zero or extremely
// small layer heights. The generated layer plan must remain usable by later FFF
// steps that expect positive layer thicknesses.
#define MIN_LAYER_HEIGHT 0.005

enum ProgressPhase : uint32_t
{
    ProgressScanVolumes = 0
};

struct LayerHeightSlicingParameters
{
    // All distances in this structure are scaled Z coordinates. Keeping the
    // whole planning code in scaled integer units avoids producing layer
    // heights that differ from the normal slicer by tiny floating point noise.
    bool first_object_layer_height_fixed = false;
    coord_t first_object_layer_height = 0;
    coord_t object_print_z_height = 0;
    coord_t z_step = 0;
    coord_t layer_height = 0;
    coord_t min_layer_height = 0;
    coord_t max_layer_height = 0;
};

struct FlatSurface
{
    // Candidate Z where the slicer may want a layer boundary. Area is stored in
    // mm2 because it is compared directly with the user-facing setting.
    coord_t z = 0;
    double area = 0.0;
};

coord_t check_z_step(coord_t val, coord_t z_step)
{
    // z_step constrains layer boundaries to printer-supported increments. The
    // rounding formula is integer-only and rounds to the nearest z_step.
    if (z_step <= SCALED_EPSILON)
        return val;
    return ((((val * 2) + z_step) / (2 * z_step)) * z_step);
}

coord_t z_step_unit(coord_t z_step)
{
    return z_step > SCALED_EPSILON ? z_step : 1;
}

uint32_t config_extruder_idx(int extruder_id)
{
    // Config values use 1-based extruder ids where 0 means "default". The
    // vector options in ConfigOption are indexed from zero.
    return extruder_id > 0 ? uint32_t(extruder_id - 1) : 0;
}

void insert_config_extruder_idx(std::set<uint16_t> &extruders, const ConfigOption &nozzle_diameter, int extruder_id)
{
    // Clamp to an existing nozzle entry. Invalid or stale extruder ids should
    // not make this planning plugin fail; they fall back to the last known
    // configured nozzle diameter, matching the defensive style of config code.
    const uint32_t count = nozzle_diameter.size();
    const uint32_t idx = std::min(config_extruder_idx(extruder_id), count > 0 ? count - 1 : 0);
    extruders.insert(uint16_t(idx));
}

std::set<uint16_t> object_extruders(Print print, Object object)
{
    // Layer height limits are constrained by every extruder that may print this
    // object. Scan all print regions and collect the extruders used by
    // perimeters, brim, sparse infill and solid infill.
    std::set<uint16_t> extruders;
    const Config print_config = print.config();
    const Config object_config = object.config();
    const ConfigOption nozzle_diameter = print_config.get("nozzle_diameter");

    for (uint32_t region_idx = 0; region_idx < object.print_region_count(); ++region_idx) {
        const PrintRegion region = object.print_region(region_idx);
        const Config region_config = region.config();

        if (region_config.get("perimeters").get_int() > 0 ||
            object_config.get("brim_width").get_float() > 0.0 ||
            object_config.get("brim_width_interior").get_float() > 0.0)
            insert_config_extruder_idx(extruders, nozzle_diameter, region_config.get("perimeter_extruder").get_int());

        if (region_config.get("fill_density").get_float() > 0.0)
            insert_config_extruder_idx(extruders, nozzle_diameter, region_config.get("infill_extruder").get_int());

        if (region_config.get("top_solid_layers").get_int() > 0 ||
            region_config.get("bottom_solid_layers").get_int() > 0 ||
            (region_config.get("solid_infill_every_layers").get_int() > 0 &&
             region_config.get("fill_density").get_float() > 0.0))
            insert_config_extruder_idx(extruders, nozzle_diameter, region_config.get("solid_infill_extruder").get_int());
    }

    return extruders;
}

coord_t min_layer_height_from_nozzle(const Config &print_config, uint32_t extruder_idx)
{
    // min_layer_height may be expressed relative to nozzle diameter. Resolve it
    // for one extruder, apply the physical minimum, then snap to z_step.
    const ConfigOption nozzle_diameter_opt = print_config.get("nozzle_diameter");
    const uint32_t nozzle_idx = extruder_idx < nozzle_diameter_opt.size() ? extruder_idx : 0;
    const double nozzle_diameter = nozzle_diameter_opt.get_float(nozzle_idx);
    const ConfigOption min_layer_height_opt = print_config.get("min_layer_height");
    const uint32_t value_idx = extruder_idx < min_layer_height_opt.size() ? extruder_idx : 0;
    double value = min_layer_height_opt.get_effective_value(nozzle_diameter, value_idx);
    if (value == 0.0)
        value = nozzle_diameter / 2.0;
    return check_z_step(scale_i(std::max(MIN_LAYER_HEIGHT, value)), scale_i(print_config.get("z_step").get_float()));
}

coord_t max_layer_height_from_nozzle(const Config &print_config, uint32_t extruder_idx)
{
    // A disabled or zero max_layer_height means the usual 75% of nozzle
    // diameter. The max must never fall below the resolved minimum for the same
    // extruder.
    const ConfigOption nozzle_diameter_opt = print_config.get("nozzle_diameter");
    const uint32_t nozzle_idx = extruder_idx < nozzle_diameter_opt.size() ? extruder_idx : 0;
    const double nozzle_diameter = nozzle_diameter_opt.get_float(nozzle_idx);
    const ConfigOption max_layer_height_opt = print_config.get("max_layer_height");
    const uint32_t value_idx = extruder_idx < max_layer_height_opt.size() ? extruder_idx : 0;
    double value = max_layer_height_opt.get_effective_value(nozzle_diameter, value_idx);
    if (value == 0.0 || !max_layer_height_opt.is_enabled(value_idx))
        value = 0.75 * nozzle_diameter;
    const coord_t min_layer_height = min_layer_height_from_nozzle(print_config, extruder_idx);
    return check_z_step(std::max(min_layer_height, scale_i(value)), scale_i(print_config.get("z_step").get_float()));
}

LayerHeightSlicingParameters make_layer_height_slicing_parameters(Print print, Object object, coord_t max_z)
{
    // Reconstruct the layer-height constraints normally owned by the host:
    // default height, first-layer behavior, object print height, z_step and the
    // tightest min/max layer height range across all relevant extruders.
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
        // Percentage first-layer height depends on nozzle diameter. Use the
        // smallest effective value among object extruders so every active
        // extruder can print the first layer.
        first_layer_height = std::numeric_limits<coord_t>::max();
        for (uint16_t extruder_idx : extruders) {
            if (extruder_idx >= nozzle_diameter_opt.size())
                continue;
            const double nozzle = nozzle_diameter_opt.get_float(extruder_idx);
            first_layer_height = std::min(first_layer_height, scale_i(first_layer_height_opt.get_effective_value(nozzle)));
        }
        if (first_layer_height == std::numeric_limits<coord_t>::max())
            first_layer_height = scale_i(first_layer_height_opt.get_effective_value(nozzle_diameter_opt.get_float(0)));
    } else {
        first_layer_height = scale_i(first_layer_height_opt.get_float());
    }
    first_layer_height = check_z_step(first_layer_height, out.z_step);
    if (first_layer_height <= SCALED_EPSILON)
        first_layer_height = out.layer_height;

    out.first_object_layer_height = first_layer_height;
    out.object_print_z_height = check_z_step(max_z, out.z_step);
    if (out.object_print_z_height + SCALED_EPSILON < max_z)
        out.object_print_z_height += out.z_step;

    coord_t min_layer_height = 0;
    coord_t max_layer_height = std::numeric_limits<coord_t>::max();

    // Supports may use extruders that are not otherwise referenced by object
    // regions. Include them so the generated layer plan is compatible with
    // support material emitted for this object. A raft alone is not object
    // geometry, so it must not change the object's layer plan.
    const bool has_support = object_config.get("support_material").get_bool() ||
        object_config.get("support_material_enforce_layers").get_int() > 0;
    if (has_support) {
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
        // Empty printable regions are still sliced with a valid default plan.
        // Use extruder 0 as the least surprising fallback.
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
    if (max_layer_height < min_layer_height)
        max_layer_height = min_layer_height;

    out.min_layer_height = check_z_step(min_layer_height, out.z_step);
    out.max_layer_height = check_z_step(max_layer_height, out.z_step);
    out.layer_height = std::clamp(out.layer_height, out.min_layer_height, out.max_layer_height);

    // The layer-height step plans object-local layers only. Raft generation is
    // a separate support concern and must not alter the first object layer.
    out.first_object_layer_height_fixed = true;

    return out;
}

c_vec3f transform_point(c_matrix4d matrix, c_vec3f point)
{
    // Apply the object+volume transform to a mesh vertex. The matrix is passed
    // through the C API, so keep the operation explicit rather than depending
    // on Eigen types in the plugin.
    const double x = matrix.value[0] * point.x + matrix.value[1] * point.y + matrix.value[2] * point.z + matrix.value[3];
    const double y = matrix.value[4] * point.x + matrix.value[5] * point.y + matrix.value[6] * point.z + matrix.value[7];
    const double z = matrix.value[8] * point.x + matrix.value[9] * point.y + matrix.value[10] * point.z + matrix.value[11];
    const double w = matrix.value[12] * point.x + matrix.value[13] * point.y + matrix.value[14] * point.z + matrix.value[15];
    const double inv_w = std::abs(w) > 1e-12 ? 1.0 / w : 1.0;
    return c_vec3f{float(x * inv_w), float(y * inv_w), float(z * inv_w)};
}

double xy_triangle_area(c_vec3f a, c_vec3f b, c_vec3f c)
{
    // Flat horizontal surfaces are measured by their XY projected area. For a
    // horizontal triangle this is the real surface area; for degenerate input
    // it naturally evaluates close to zero and is ignored by the caller.
    const double abx = double(b.x) - double(a.x);
    const double aby = double(b.y) - double(a.y);
    const double acx = double(c.x) - double(a.x);
    const double acy = double(c.y) - double(a.y);
    return 0.5 * std::abs(abx * acy - aby * acx);
}

bool triangle_is_horizontal(c_vec3f a, c_vec3f b, c_vec3f c)
{
    // A candidate anchor must come from a real horizontal facet. Tiny Z
    // differences are tolerated to avoid dropping faces because of mesh import
    // precision.
    const double min_z = std::min({ double(a.z), double(b.z), double(c.z) });
    const double max_z = std::max({ double(a.z), double(b.z), double(c.z) });
    return max_z - min_z <= EPSILON;
}

void collect_flat_surfaces_from_volume(const Volume &volume,
                                       c_matrix4d object_transform,
                                       const LayerHeightSlicingParameters &params,
                                       std::map<coord_t, double> &area_by_z)
{
    // Only model-part volumes define printable geometry. Modifier volumes and
    // support-enforcer/blocker volumes may also contain horizontal triangles,
    // but they must not create layer-height anchors on their own.
    if (!volume.is_model_part())
        return;

    const TriangleMesh mesh = volume.mesh();
    if (mesh.empty())
        return;

    const c_matrix4d transform = matrix4d_mul(object_transform, volume.matrix());
    for (uint32_t triangle_idx = 0; triangle_idx < mesh.triangle_count(); ++triangle_idx) {
        const c_triangle_indices indices = mesh.triangle(triangle_idx);
        const c_vec3f a = transform_point(transform, mesh.vertex(indices.a));
        const c_vec3f b = transform_point(transform, mesh.vertex(indices.b));
        const c_vec3f c = transform_point(transform, mesh.vertex(indices.c));
        if (!triangle_is_horizontal(a, b, c))
            continue;

        const double area = xy_triangle_area(a, b, c);
        if (area <= EPSILON)
            continue;

        const double z_mm = (double(a.z) + double(b.z) + double(c.z)) / 3.0;
        const coord_t z = check_z_step(scale_to_layer_coord(z_mm), params.z_step);
        // Do not anchor on the build plate or on the final object top. Those
        // bounds are already mandatory anchors and adding them as candidates
        // would only duplicate work.
        if (z <= SCALED_EPSILON || z >= params.object_print_z_height - SCALED_EPSILON)
            continue;
        // When the first object layer height is fixed, the first internal flat
        // surface cannot move the first boundary. Ignore candidates inside that
        // protected first-layer interval.
        if (params.first_object_layer_height_fixed && z <= params.first_object_layer_height + SCALED_EPSILON)
            continue;

        // Multiple coplanar triangles, possibly from several volumes, are
        // accumulated into one Z candidate. The user setting is based on the
        // total horizontal area visible at that height.
        area_by_z[z] += area;
    }
}

std::vector<FlatSurface> collect_flat_surfaces(Object object,
                                               const LayerHeightSlicingParameters &params,
                                               double min_flat_area,
                                               PluginProgress &progress)
{
    std::map<coord_t, double> area_by_z;
    const c_matrix4d object_transform = object.transform_centered();
    for (uint32_t volume_idx = 0; volume_idx < object.volume_count(); ++volume_idx) {
        const Volume volume = object.volume(volume_idx);
        collect_flat_surfaces_from_volume(volume, object_transform, params, area_by_z);
        progress.increment(ProgressScanVolumes);
    }

    std::vector<FlatSurface> flat_surfaces;
    flat_surfaces.reserve(area_by_z.size());
    for (const std::pair<const coord_t, double> &entry : area_by_z) {
        // Small shelves and tiny mesh artifacts would make the optimizer chase
        // too many anchors. Filter them before sorting candidates by priority.
        if (entry.second >= min_flat_area)
            flat_surfaces.push_back(FlatSurface{entry.first, entry.second});
    }

    std::sort(flat_surfaces.begin(), flat_surfaces.end(), [](const FlatSurface &lhs, const FlatSurface &rhs) {
        // Larger flat areas are more valuable to hit exactly. Ties are resolved
        // bottom-up to keep output stable for regression tests.
        if (lhs.area != rhs.area)
            return lhs.area > rhs.area;
        return lhs.z < rhs.z;
    });
    return flat_surfaces;
}

int64_t ceil_div(int64_t lhs, int64_t rhs)
{
    assert(rhs > 0);
    return (lhs + rhs - 1) / rhs;
}

bool interval_is_fixed_first_layer(coord_t lo, coord_t hi, const LayerHeightSlicingParameters &params)
{
    // The first object layer may be fixed by profile semantics. Treat this
    // interval as already valid even if it violates the normal min/max range.
    return params.first_object_layer_height_fixed && lo == 0 && hi == params.first_object_layer_height;
}

bool interval_can_be_layered(coord_t lo, coord_t hi, const LayerHeightSlicingParameters &params)
{
    // Test whether an interval between two mandatory Z anchors can be filled by
    // at least one integer number of layers inside the configured min/max
    // height range. This is the key guard that keeps candidate anchors from
    // making the final layer plan impossible.
    if (hi <= lo)
        return true;
    if (interval_is_fixed_first_layer(lo, hi, params))
        return true;

    const coord_t unit = z_step_unit(params.z_step);
    const int64_t distance = int64_t((hi - lo) / unit);
    const int64_t min_height = std::max<int64_t>(1, int64_t(params.min_layer_height / unit));
    const int64_t max_height = std::max(min_height, int64_t(params.max_layer_height / unit));
    const int64_t min_layers = ceil_div(distance, max_height);
    const int64_t max_layers = distance / min_height;
    return min_layers <= max_layers;
}

bool anchors_can_be_layered(const std::vector<coord_t> &anchors, const LayerHeightSlicingParameters &params)
{
    // Anchors are accepted only if every consecutive interval can later be
    // expanded into real layer boundaries.
    for (size_t idx = 1; idx < anchors.size(); ++idx)
        if (!interval_can_be_layered(anchors[idx - 1], anchors[idx], params))
            return false;
    return true;
}

std::vector<coord_t> select_flat_surface_anchors(const std::vector<FlatSurface> &flat_surfaces,
                                                 const LayerHeightSlicingParameters &params)
{
    // Start with anchors that are always required: bed, object top, and
    // optionally the fixed first object layer. Candidate flat surfaces are then
    // tried greedily from largest area to smallest.
    std::vector<coord_t> anchors;
    anchors.push_back(0);
    if (params.first_object_layer_height_fixed)
        anchors.push_back(params.first_object_layer_height);
    anchors.push_back(params.object_print_z_height);
    std::sort(anchors.begin(), anchors.end());
    anchors.erase(std::unique(anchors.begin(), anchors.end()), anchors.end());

    for (const FlatSurface &surface : flat_surfaces) {
        if (std::binary_search(anchors.begin(), anchors.end(), surface.z))
            continue;

        // A candidate is kept only if adding it still leaves every interval
        // layerable. This simple greedy approach favors the largest flat areas
        // without needing a costly global optimization pass.
        std::vector<coord_t> candidate = anchors;
        candidate.push_back(surface.z);
        std::sort(candidate.begin(), candidate.end());
        if (anchors_can_be_layered(candidate, params))
            anchors = std::move(candidate);
    }

    return anchors;
}

void append_interval_layers(std::vector<coord_t> &boundaries,
                            coord_t lo,
                            coord_t hi,
                            const LayerHeightSlicingParameters &params)
{
    // Expand one anchor-to-anchor interval into concrete layer boundaries. The
    // generated heights are as close as possible to the preferred layer height,
    // but they must stay inside min/max and on the z_step grid.
    if (hi <= lo)
        return;

    if (boundaries.empty())
        boundaries.push_back(lo);
    assert(boundaries.back() == lo);

    if (interval_is_fixed_first_layer(lo, hi, params)) {
        // Preserve the exact first layer interval. Later intervals may adapt,
        // but the first object layer is user-visible and must not be smoothed.
        boundaries.push_back(hi);
        return;
    }

    const coord_t unit = z_step_unit(params.z_step);
    const int64_t distance = int64_t((hi - lo) / unit);
    const int64_t min_height = std::max<int64_t>(1, int64_t(params.min_layer_height / unit));
    const int64_t max_height = std::max(min_height, int64_t(params.max_layer_height / unit));
    const int64_t preferred_height = std::clamp<int64_t>(int64_t(params.layer_height / unit), min_height, max_height);
    const int64_t min_layers = ceil_div(distance, max_height);
    const int64_t max_layers = distance / min_height;
    if (min_layers > max_layers) {
        // This should be rare because anchors are pre-validated, but keep a
        // robust fallback in case settings changed or rounding collapsed an
        // interval. The host can still slice the object with a direct boundary.
        boundaries.push_back(hi);
        return;
    }

    // Pick the layer count closest to the preferred height, then distribute the
    // z_step-sized remainder over the first layers. This guarantees that the
    // final boundary lands exactly on hi without accumulating drift.
    const int64_t preferred_layers = std::max<int64_t>(1, int64_t(std::llround(double(distance) / double(preferred_height))));
    const int64_t layer_count = std::clamp(preferred_layers, min_layers, max_layers);
    const int64_t base_height = distance / layer_count;
    const int64_t remainder = distance % layer_count;

    coord_t current = lo;
    for (int64_t layer_idx = 0; layer_idx < layer_count; ++layer_idx) {
        const int64_t height_units = base_height + (layer_idx < remainder ? 1 : 0);
        current += coord_t(height_units * unit);
        boundaries.push_back(current);
    }
    boundaries.back() = hi;
}

std::vector<coord_t> make_layer_boundaries(const std::vector<coord_t> &anchors,
                                           const LayerHeightSlicingParameters &params)
{
    // Convert mandatory anchors into the full sorted list of layer boundary Zs.
    // Each interval is independent, so every selected flat surface remains an
    // exact layer boundary in the final layer plan.
    std::vector<coord_t> boundaries;
    boundaries.reserve(anchors.size() * 2);
    boundaries.push_back(anchors.front());
    for (size_t idx = 1; idx < anchors.size(); ++idx)
        append_interval_layers(boundaries, anchors[idx - 1], anchors[idx], params);
    boundaries.erase(std::unique(boundaries.begin(), boundaries.end()), boundaries.end());
    return boundaries;
}

std::vector<coord_t> layer_descriptors_from_boundaries(const std::vector<coord_t> &boundaries,
                                                       const LayerHeightSlicingParameters &params)
{
    // Translate explicit layer boundaries into the STEP_LAYER_HEIGHT payload:
    // [layer_top_z, layer_height, ...]. Boundaries normally contain at least
    // the bed and the object top because select_flat_surface_anchors() always
    // seeds both. The fallback keeps the plugin usable even if a future caller
    // passes an empty or collapsed boundary list.
    std::vector<coord_t> usable_boundaries = boundaries;
    if (usable_boundaries.size() < 2) {
        usable_boundaries.clear();
        usable_boundaries.push_back(0);
        append_interval_layers(usable_boundaries, 0, params.object_print_z_height, params);
    }

    std::vector<coord_t> descriptors;
    descriptors.reserve(usable_boundaries.size() * 2);
    for (size_t idx = 1; idx < usable_boundaries.size(); ++idx) {
        const coord_t lo = usable_boundaries[idx - 1];
        const coord_t hi = usable_boundaries[idx];
        const coord_t height = hi - lo;
        if (height <= 0)
            continue;
        descriptors.push_back(hi);
        descriptors.push_back(height);
    }
    return descriptors;
}

std::vector<coord_t> make_flat_area_layer_descriptors(Object object,
                                                      const LayerHeightSlicingParameters &params,
                                                      double min_flat_area,
                                                      PluginProgress &progress)
{
    // Main algorithm:
    // 1. collect horizontal surface candidates from model volumes;
    // 2. greedily keep the largest candidates that still allow valid layers;
    // 3. fill every accepted interval with legal layer heights.
    // The host receives explicit [layer_top_z, layer_height] pairs. This keeps
    // enough information to represent non-contiguous layers if a future
    // generator needs to leave an empty interval.
    std::vector<FlatSurface> flat_surfaces = collect_flat_surfaces(object, params, min_flat_area, progress);
    std::vector<coord_t> anchors = select_flat_surface_anchors(flat_surfaces, params);
    std::vector<coord_t> boundaries = make_layer_boundaries(anchors, params);
    return layer_descriptors_from_boundaries(boundaries, params);
}

} // namespace

FlatAreaLayerHeight &FlatAreaLayerHeight::instance(orchestrator_handle *orch)
{
    static FlatAreaLayerHeight s_instance(orch);
    return s_instance;
}

const char *FlatAreaLayerHeight::id_impl() const noexcept
{
    return k_flat_area_layer_height_id;
}

const char *FlatAreaLayerHeight::name_impl() const noexcept
{
    return "Flat area layer height";
}

const char *FlatAreaLayerHeight::description_impl() const noexcept
{
    return "Place layer boundaries on large horizontal mesh areas when possible.";
}

slicing_step_t FlatAreaLayerHeight::step_impl() const noexcept
{
    return STEP_LAYER_HEIGHT;
}

const char *const *FlatAreaLayerHeight::dependencies_impl() const noexcept
{
    return k_no_dependencies;
}

int32_t FlatAreaLayerHeight::priority_impl() const noexcept
{
    return 10;
}

int32_t FlatAreaLayerHeight::used_config_keys(raw_used_config_key *keys) const noexcept
{
    // The exclusive-step GUI uses this list to disable settings that are only
    // meaningful when this layer-height plugin is selected.
    if (keys != nullptr)
        keys[0] = k_used_config_keys[0];
    return 1;
}

int32_t FlatAreaLayerHeight::defined_config_keys(const char **keys) const noexcept
{
    if (keys != nullptr)
        keys[0] = k_defined_config_keys[0];
    return 1;
}

const char *FlatAreaLayerHeight::progress_message_format_impl() const noexcept
{
    return "Scanning flat surfaces: %u / %u volumes";
}

const char *FlatAreaLayerHeight::print_ui_fragment() noexcept
{
    return "page:Slicing\n"
           "group:Layer height\n"
           "line:Flat area layer matching\n"
           "setting:layer_height_min_flat_area\n"
           "end_line\n";
}

void FlatAreaLayerHeight::inilialize_impl(storage_handle *) const
{
    // Register the only setting owned by this plugin. The option lives in the
    // project/FFF print config, so it can be saved in presets and read during
    // STEP_LAYER_HEIGHT before object layers are created.
    raw_config_option_def def = raw_config_option_def_init();
    def.opt_key = "layer_height_min_flat_area";
    def.type = RAW_CO_FLOAT;
    def.container_type = RAW_CONTAINER_TYPE_PROJECT;
    def.option_preset_type = RAW_PRESET_TYPE_FFF_PRINT;
    def.printer_technology = RAW_PT_FFF;
    def.label = "Min flat area";
    def.full_label = "Minimum flat area for layer matching";
    def.category = RAW_OPTION_CATEGORY_SLICING;
    def.level = RAW_OPTION_LEVEL_ADVANCED;
    def.tooltip = "Horizontal mesh surfaces whose total area at a Z is below this value are ignored by the flat-area layer height plugin.";
    def.sidetext = "mm2";
    def.has_min = 1;
    def.min_value = 0.0;
    def.precision = 3;
    def.mode = RAW_CONFIG_OPTION_MODE_ADV_EXP | RAW_CONFIG_OPTION_MODE_SUSI;
    def.default_serialized_value = "1";
    orchestrator_create_option_def(m_orchestrator, &def);

    orchestrator_add_ui_fragment(m_orchestrator,
                                 "print.ui",
                                 k_flat_area_layer_height_id,
                                 FlatAreaLayerHeight::print_ui_fragment(),
                                 0);
}

void FlatAreaLayerHeight::setup_run_impl(const plugin_run_context *run_ctx) const
{
    const run_ctx_layer_height_generation *ctx = plugin_ctx_as_layer_height_generation(run_ctx);
    if (ctx == nullptr || ctx->object == nullptr)
        return;
    // Progress is volume-based because the expensive part of this plugin is the
    // mesh scan. The later planning work is small compared to walking every
    // triangle of every model volume.
    progress().add_max(ProgressScanVolumes, Object(ctx->object).volume_count());
}

void FlatAreaLayerHeight::run_impl(const plugin_run_context *run_ctx) const
{
    const run_ctx_layer_height_generation *ctx = plugin_ctx_as_layer_height_generation(run_ctx);
    if (ctx == nullptr || ctx->print == nullptr || ctx->object == nullptr || ctx->set_layer_height_profile == nullptr)
        return;

    if (ctx->enforce_layer_zs != nullptr && ctx->enforce_layer_zs_size > 0) {
        // The host may pass mandatory layer positions, for example from an
        // imported project or another upstream decision. Those positions have
        // higher priority than this plugin's heuristic.
        ctx->set_layer_height_profile(ctx->object, ctx->enforce_layer_zs, ctx->enforce_layer_zs_size);
        progress().finish_run(ProgressScanVolumes);
        return;
    }

    const Print print(ctx->print);
    const Object object(ctx->object);
    const Config print_config = print.config();
    const double min_flat_area = print_config.get("layer_height_min_flat_area").get_float();
    const LayerHeightSlicingParameters params = make_layer_height_slicing_parameters(print, object, ctx->max_z);
    std::vector<coord_t> layer_descriptors =
        make_flat_area_layer_descriptors(object, params, min_flat_area, progress());

    // A non-empty descriptor list is transferred as borrowed memory for the
    // duration of the callback only; the host copies it before returning from
    // set_layer_height_profile().
    if (!layer_descriptors.empty())
        ctx->set_layer_height_profile(ctx->object, layer_descriptors.data(), uint32_t(layer_descriptors.size()));
    else
        ctx->set_layer_height_profile(ctx->object, nullptr, 0);

    progress().finish_run(ProgressScanVolumes);
}

void register_flat_area_layer_height_plugin(orchestrator_handle *orch)
{
    orchestrator_register_plugin(orch, FlatAreaLayerHeight::instance(orch).c_instance());
}

}} // namespace slic3r_api::FlatAreaLayerHeightPlugin

#ifdef FLAT_AREA_LAYER_HEIGHT_PLUGIN_DLL
#include "libslic3r/Api/plugin/c/slic3r_plugin_register_version.h"

extern "C" SLIC3R_PLUGIN_API void register_plugin(orchestrator_handle *orch)
{
    slic3r_api::FlatAreaLayerHeightPlugin::register_flat_area_layer_height_plugin(orch);
}
#endif // FLAT_AREA_LAYER_HEIGHT_PLUGIN_DLL
