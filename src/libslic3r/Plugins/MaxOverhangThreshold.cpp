///|/ Copyright (c) SuperSlicer 2026 Durand Rémi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/

#include "MaxOverhangThreshold.hpp"

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <set>

#include "libslic3r/Api/plugin/c/slic3r_config_def.h"
#include "libslic3r/Api/plugin/c/slic3r_orchestrator.h"
#include "libslic3r/Api/plugin/c/steps/slic3r_step_post_slicing.h"
#include "libslic3r/Api/plugin/cpp/PrintHelpers.hpp"
#include "libslic3r/Api/plugin/cpp/Views.hpp"

/*
Maximum overhang-slope post-processing
======================================

This plugin provides the STEP_POST_SLICING pass that enlarges supported raw
slices according to the configured maximum overhang slope. It works before
surface and perimeter generation, so it changes LayerRegion raw slices rather
than already-generated extrusion paths.

The plugin runs once per PrintObject. For every layer after the first, it
builds the area supported by the previous layer, detects portions that can be
bridged, optionally looks ahead through upper layers for future bridges, and
expands the supported mask by the configured slope distance. Bridgeable areas
are restored to the mask without being enlarged, so bridge geometry remains
available for later processing. The expanded mask is intersected with each
region's original slices and written back through the mutable step context.

After all regions of a layer have been processed, the host is asked to rebuild
the layer slices and islands from the updated LayerRegion slices. This keeps
the data consumed by later steps consistent with the raw-slice changes.

The normal call flow is:

    MaxOverhangThreshold::inilialize_impl()
    `-- create plugin settings and GUI rules

    MaxOverhangThreshold::setup_run_impl()
    `-- reserve progress for layers after layer zero

    MaxOverhangThreshold::run_impl()
    `-- resolve PrintObject and shared nozzle/resolution data
        |-- no region enables overhangs_max_slope?
        |   `-- finish without changing slices
        `-- for each object layer after the first
            |-- intersect current and lower-layer slices
            |-- for each region
            |   |-- detect current-layer bridgeable areas
            |   |-- optionally detect bridgeable areas in upper layers
            |   |-- enlarge the supported mask by the maximum slope
            |   |-- restore bridge areas without enlargement
            |   `-- write replacement raw slices to the LayerRegion
            `-- layer_recompute_slices_and_islands_from_layer_region()

The first layer and raft-covered layers are not enlarged. A disabled or zero
maximum slope leaves the corresponding region unchanged. The plugin owns the
raw-slice transformation and cache-rebuild request; later pipeline steps own
the regenerated surfaces and perimeters.
*/

namespace slic3r_api { namespace MaxOverhangThresholdPlugin {

namespace {

const char *k_max_overhang_threshold_id = "max_overhang_threshold";
const char *k_no_dependencies[] = {nullptr};
const raw_used_config_key k_used_config_keys[] = {
    { "overhangs_bridge_threshold", RAW_CO_FLOAT, RAW_CONTAINER_TYPE_REGION, RAW_PRESET_TYPE_FFF_PRINT },
    { "overhangs_bridge_upper_layers", RAW_CO_INT, RAW_CONTAINER_TYPE_REGION, RAW_PRESET_TYPE_FFF_PRINT },
    { "overhangs_max_slope", RAW_CO_FLOAT_OR_PERCENT, RAW_CONTAINER_TYPE_REGION, RAW_PRESET_TYPE_FFF_PRINT },
    { "bridge_precision", RAW_CO_FLOAT, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE },
    { "nozzle_diameter", RAW_CO_VECTOR_FLOAT, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE },
    { "resolution", RAW_CO_FLOAT, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE }
};
constexpr size_t k_defined_config_key_count = 3;

uint32_t layer_work_count(const Object &object)
{
    // Layer 0 has no lower layer, so the overhang pass starts at layer 1.
    const size_t layer_count = object.layer_count();
    return layer_count > 1 ? static_cast<uint32_t>(layer_count - 1) : 0;
}

} // namespace

// Remove concave spikes sharper than min_angle from a mutable polygon view.
// This is a direct helper for the legacy _max_overhang_threshold algorithm and
// is tuned for the default 90-degree case.
template<class Poly> void only_convex_or_gt(Poly &poly, const double min_angle = PI / 2) {
    const bool ccw = poly.is_counter_clockwise();
    std::vector<uint32_t> concave = ccw ? poly.concave_points_idx(0, min_angle - 0.001) :
                                          poly.convex_points_idx(0, min_angle - 0.001);
    size_t iter = 0;
    while (!concave.empty()) {
        assert(std::is_sorted(concave.begin(), concave.end()));
        std::vector<c_point> new_pts;
        bool previous_modified = false;
        for (uint32_t idx = 0; idx < poly.size(); ++idx) {
            if (previous_modified || std::find(concave.begin(), concave.end(), idx) == concave.end()) {
                // convex: keep
                new_pts.push_back(poly[idx]);
                previous_modified = false;
            } else {
                previous_modified = true;
                // Replace the concave point by a point projected onto the longer
                // adjacent side, keeping the smaller side as the angle reference.
                c_point small_side_point = idx == 0 ? poly.back() : poly[idx - 1];
                c_point big_side_point = idx == poly.size() - 1 ? poly.front() : poly[idx + 1];
                if (norm_square(poly[idx] - small_side_point) > norm_square(poly[idx] - big_side_point)) {
                    big_side_point = idx == 0 ? poly.back() : poly[idx - 1];
                    small_side_point = idx == poly.size() - 1 ? poly.front() : poly[idx + 1];
                }
                // then get the distance to move in the big side
                c_point previous_point = ccw ? (idx == 0 ? poly.back() : poly[idx - 1]) :
                                               (idx == poly.size() - 1 ? poly.front() : poly[idx + 1]);
                c_point next_point = ccw ? (idx == poly.size() - 1 ? poly.front() : poly[idx + 1]) :
                                           (idx == 0 ? poly.back() : poly[idx - 1]);
                double angle = abs_angle(angle_ccw(previous_point - poly[idx], next_point - poly[idx]));
                if (angle < min_angle + 0.001 && angle > min_angle) {
                    angle = min_angle;
                }
                assert(angle <= min_angle + EPSILON && angle >= 0);
                coordf_t dist_to_move = std::cos(angle) * norm(poly[idx] - small_side_point) + SCALED_EPSILON / 2;
                // Increase the move distance on repeated iterations. Otherwise
                // very narrow sawtooth chains can take too many tiny steps.
                dist_to_move *= (0.95 +
                                 ((iter + 2) * (iter + 1) / 40)); // 0->1; 1->1.1; 3->1.45; 5->2; 10->4.2; 20->12.5
                // If the projected point would pass the opposite vertex, deleting
                // the original point is safer than creating an inverted segment.
                if (dist_to_move < norm(poly[idx] - big_side_point)) {
                    c_point new_pt = point_at(poly[idx], big_side_point, dist_to_move);
                    new_pts.push_back(new_pt);
                    double angle_new = abs_angle(angle_ccw(previous_point - new_pt, next_point - new_pt));
                    assert(angle_new != angle);
                }
            }
        };
        poly.clear();
        poly.insert_array(0, new_pts.data(), new_pts.size());
        concave = ccw ? poly.concave_points_idx(0, min_angle - 0.001) : poly.convex_points_idx(0, min_angle - 0.001);

        if (iter > 20) {
            // Abort where we are: too many iterations means the polygon is too
            // pathological for this local cleanup pass.
            return;
        }
        iter++;
    }
}

// Remove small zig-zag segments after support enlargement. This keeps enlarged
// areas from producing very short alternating edges around bridge transitions.
template<class Poly>
void smoothen(Poly &poly, const distf_t max_length = scale_d(1), const double threshold = PI / 2) {
    // for each short-enough segment
    distsqrf_t max_d2 = max_length * max_length;
    distsqrf_t min_d2 = (max_length / 20) * (max_length / 20);
    bool previous_culled = false;
    bool prev_angle_culled = 0;
    for (uint32_t i = 0; i < poly.size(); i++) {
        // remove too small sections
        uint32_t im1 = std::min(i - 1, poly.size() - 1);
        distsqrf_t my_d2 = norm_square(poly[im1] - poly[i]);
        if (min_d2 > my_d2) {
            poly.set(im1, midpoint(poly[im1], poly[i]));
            poly.erase(i);
            i--;
        }
    }
    min_d2 = (max_length / 5) * (max_length / 5);
    for (uint32_t i = 0; i < poly.size(); i++) {
        bool next_culled = false;
        if (poly.size() <= 3) {
            // polygon too small
            return;
        }
        // i % poly.size()
        uint32_t im1 = std::min(i - 1, poly.size() - 1);
        distsqrf_t my_d2 = norm_square(poly[im1] - poly[i]);
        if (max_d2 > my_d2) {
            bool too_short = min_d2 > my_d2;
            // if previous angle is sharp convex/concave
            uint32_t im2 = std::min(i - 2, poly.size() - 2);
            double prev_angle = previous_culled ? prev_angle_culled :
                                                  angle_ccw(poly[im2] - poly[im1], poly[i] - poly[im1]);
            // if next angle is sharp concave/convex
            uint32_t ip1 = ((i + 1) < poly.size() ? i + 1 : 0);
            double next_angle = angle_ccw(poly[im1] - poly[i], poly[ip1] - poly[i]);
            if ((prev_angle < 0 && next_angle > 0) || (prev_angle > 0 && next_angle < 0)) {
                // then merge the segment into a point
                if (too_short || (std::abs(prev_angle) < threshold && std::abs(next_angle) < threshold)) {
                    c_point mid = slic3r_api::midpoint(poly[im1], poly[i]);
                    // smoothen
                    if (norm_square(poly[im2] - poly[im1]) > max_d2) {
                        poly.set(im1, point_at(poly[im1], poly[im2], max_length));
                        if (norm_square(poly[i] - poly[ip1]) > max_d2) {
                            poly.set(i, point_at(poly[i], poly[ip1], max_length));
                        } else {
                            poly.set(i, mid);
                        }
                    } else {
                        poly.set(im1, mid);
                        if (norm_square(poly[i] - poly[ip1]) > max_d2) {
                            poly.set(i, point_at(poly[i], poly[ip1], max_length));
                        } else {
                            poly.erase(i);
                            i--;
                        }
                    }
                    next_culled = true;
                    prev_angle_culled = next_angle;
                }
            }
        }
        prev_angle_culled = next_culled;
    }
}

void MaxOverhangThreshold::run_impl(const plugin_run_context *run_ctx) const {
    // POST_SLICING exposes raw slices as mutable borrowed handles. The plugin
    // materializes temporary ClipperOperands in plugin storage, writes final raw
    // slices back to LayerRegions, then asks the host to rebuild layer caches.
    const run_ctx_post_slicing *ctx = plugin_ctx_as_post_slicing(run_ctx);
    if (ctx == nullptr || ctx->print == nullptr || ctx->object == nullptr)
        return;

    Print print(ctx->print);
    Object object(ctx->object);
    storage_handle *storage = run_ctx->plugin_storage;
    ClipperContext clipper(storage);
    bool has_enlargment = false;

    coord_t max_nz_diam = 0;
    for (int16_t extr_id : object_extruders(print, object)) {
        max_nz_diam = std::max(max_nz_diam, scale_i(print.config().get("nozzle_diameter").get_float(extr_id)));
    }
    if (max_nz_diam == 0)
        max_nz_diam = scale_i(0.4);

    for (size_t region_idx = 0; region_idx < object.print_region_count(); ++region_idx) {
        // A region opt-in is enough to run the pass. Individual layers/regions
        // are filtered again later to avoid unnecessary Clipper work.
        coord_t enlargement = scale_i(object.print_region(region_idx)
                                          .config()
                                          .get("overhangs_max_slope")
                                          .get_effective_value(unscaled(max_nz_diam)));
        if (enlargement > 0) {
            has_enlargment = true;
            break;
        }
    }
    if (!has_enlargment)
    {
        progress().increment_by(static_cast<int32_t>(layer_work_count(object)));
        progress().finish_run();
        return;
    }

    const coord_t resolution = std::max(scale_i(print.config().get("resolution").get_float() / 2), SCALED_EPSILON);

    for (size_t layer_idx = 1; layer_idx < object.layer_count(); layer_idx++) {
        // Area already supported by the lower layer. This is shared by all
        // regions on the current layer.
        Layer my_layer = object.layer(layer_idx);
        const Layer lower_layer = object.layer(layer_idx - 1);
        assert(lower_layer.same_handle(my_layer.lower_layer()));
        bool layer_has_changed = false;

        // Skip layers where no region enables max-slope overhang enlargement.
        bool has_modifications = false;
        for (size_t lregion_idx = 0; lregion_idx < my_layer.region_count(); ++lregion_idx) {
            if (my_layer.region(lregion_idx).print_region().config().get("overhangs_max_slope").get_effective_value(1.) >
                0) {
                has_modifications = true;
                break;
            }
        }
        if (!has_modifications) {
            progress().increment();
            continue;
        }

        // const ExPolygons supported_area = ensure_valid(intersection_ex(my_layer.slices(), lower_layer.slices()),
        // resolution);
        ClipperOperand supported_area_co = clipper_intersection(clipper(my_layer.slices()), clipper(lower_layer.slices()));
        // TODO: ensure_valid/clean supported_area_co

        // Detect bridgeable unsupported areas first. Bridges are added back to
        // the supported mask but are not enlarged by the max-slope operation.
        for (size_t lregion_idx = 0; lregion_idx < my_layer.region_count(); ++lregion_idx) {
            LayerRegion lregion = my_layer.region(lregion_idx);
            c_flow bridge_flow = lregion.flow(RAW_EXTRUSION_ROLE_INFILL | RAW_EXTRUSION_ROLE_SOLID |
                                              RAW_EXTRUSION_ROLE_BRIDGE);
            ClipperOperand bridged_area_co = clipper();
            ClipperOperand bridged_other_layers_areas_co = clipper();

            Config region_config = lregion.print_region().config();
            // Bridge detection is enabled either by a non-zero explicit limit or
            // by the option being disabled, which means "unlimited" in legacy
            // semantics.
            if (region_config.get("overhangs_bridge_threshold").get_float() != 0 ||
                !region_config.get("overhangs_bridge_threshold").is_enabled()) {
                ClipperOperand unsupported_co = clipper_diff_with_safety_offset(clipper(lregion.slices()),
                                                                                clipper(lower_layer.slices()));

                if (!unsupported_co.empty()) {
                    // Remove tiny unsupported islands before asking the bridge
                    // detector; very small islands are noise for angle detection.
                    ClipperOperand unsupported_filtered_co = clipper_offset2(unsupported_co, double(-max_nz_diam / 2),
                                                                             double(max_nz_diam), CLIPPER_JOIN_MITER,
                                                                             5.);
                    unsupported_filtered_co = clipper_intersection(unsupported_co, unsupported_filtered_co);
                    unsupported_filtered_co.for_each_expolygon([&](const ExPolygon &to_bridge) {
                        // create a bridge detector
                        bridge_detector_create_input input = {};
                        input.expolygon = to_bridge.handle();
                        input.lower_slices = lower_layer.slices().handle();
                        input.spacing = bridge_flow.spacing;
                        input.precision = scale_i(
                            print.config().get("bridge_precision").get_effective_value(unscaled(bridge_flow.spacing)));
                        input.layer_id = layer_idx;
                        // BridgeDetector is a service plugin. The orchestrator
                        // returns an instance through the ABI, and the C++ view
                        // owns only that service handle, not the source geometry.
                        slic3r_api::BridgeDetector detector(orchestrator_create_bridge_detector(nullptr, &input));
                        if (region_config.get("overhangs_bridge_threshold").is_enabled()) {
                            detector.set_max_bridge_length(
                                scale_d(std::max(0., region_config.get("overhangs_bridge_threshold").get_float())));
                        } else {
                            detector.set_max_bridge_length(-1);
                        }
                        if (detector.detect_angle()) {
                            StoredPolygonCollection coverage_polygons(storage);
                            if (detector.coverage(coverage_polygons) > 0) {
                                bridged_area_co += clipper_union(clipper(coverage_polygons));
                            }
                        }
                    });
                    // Optional look-ahead: if upper layers can bridge over this
                    // layer, do not enlarge support into areas that should remain
                    // open for those bridges.
                    if (region_config.get("overhangs_bridge_upper_layers")
                            .is_enabled()) { // disabled -> don't check other layers
                        uint32_t max_layer_idx = uint32_t(
                            region_config.get("overhangs_bridge_upper_layers").get_int());
                        if (max_layer_idx == 0) // 0 -> all layers
                            max_layer_idx = object.layer_count();
                        max_layer_idx += layer_idx;
                        max_layer_idx = std::min(max_layer_idx, object.layer_count());
                        // Only look ahead from areas not already bridgeable on
                        // the current layer.
                        ClipperOperand still_unsupported_co = clipper_diff(unsupported_co, bridged_area_co);
                        {
                            ClipperOperand still_unsupported_offseted_co =
                                clipper_offset2(still_unsupported_co, double(-bridge_flow.spacing / 2),
                                                double(bridge_flow.spacing), CLIPPER_JOIN_MITER, 5.);
                            still_unsupported_co = clipper_intersection(still_unsupported_co,
                                                                        still_unsupported_offseted_co);
                        }
                        // Support available for upper bridge detection excludes
                        // the future enlarged area, which has not been computed.
                        ClipperOperand previous_supported_co = clipper();
                        previous_supported_co += supported_area_co;
                        previous_supported_co += bridged_area_co;
                        previous_supported_co = clipper_union_with_safety_offset(previous_supported_co);
                        for (size_t other_layer_bridge_idx = layer_idx + 1; other_layer_bridge_idx < max_layer_idx;
                             other_layer_bridge_idx++) {
                            // Remove voids introduced by intermediate layers
                            // before checking whether the remaining area bridges.
                            still_unsupported_co =
                                clipper_intersection(still_unsupported_co,
                                                     clipper(object.layer(other_layer_bridge_idx).slices()));
                            // Detect newly bridgeable areas on the upper layer.
                            ClipperOperand new_bridged_area_co = clipper();
                            for (size_t other_region_idx = 0; other_region_idx < my_layer.region_count();
                                 ++other_region_idx) {
                                LayerRegion other_lregion = my_layer.region(other_region_idx);
                                Config other_region_config = other_lregion.print_region().config();
                                if ((other_region_config.get("overhangs_bridge_threshold").get_float() != 0 ||
                                     !other_region_config.get("overhangs_bridge_threshold").is_enabled()) &&
                                    other_region_config.get("overhangs_max_slope").get_float() > 0) {
                                    coord_t enlargement = scale_i(
                                        region_config.get("overhangs_max_slope")
                                            .get_effective_value(unscaled(max_nz_diam))); // me or other?
                                    enlargement = std::max(enlargement, max_nz_diam);
                                    ClipperOperand other_to_bridge_co =
                                        clipper_intersection(still_unsupported_co, clipper(other_lregion.slices()));
                                    other_to_bridge_co.for_each_expolygon([&](const ExPolygon &other_to_bridge) {
                                        // Collapse too-small areas before bridge
                                        // detection; they cannot produce useful
                                        // bridge coverage and create false work.
                                        ClipperOperand test_empty_co = clipper_offset(clipper(other_to_bridge),
                                                                                      -enlargement);
                                        if (test_empty_co.empty()) {
                                            return; // continue;
                                        }
                                        // The detector needs materialized lower
                                        // slices because previous_supported_co is
                                        // an owned ClipperOperand.
                                        bridge_detector_create_input input = {};
                                        input.expolygon = other_to_bridge.handle();
                                        StoredExPolygonCollection previous_supported = previous_supported_co
                                                                                           .to_expolygon_collection();
                                        input.lower_slices = previous_supported.handle();
                                        input.spacing = bridge_flow.spacing;
                                        input.precision = scale_i(
                                            print.config()
                                                .get("bridge_precision")
                                                .get_effective_value(unscaled(bridge_flow.spacing)));
                                        input.layer_id = other_layer_bridge_idx;
                                        slic3r_api::BridgeDetector detector(
                                            orchestrator_create_bridge_detector(nullptr, &input));
                                        if (other_region_config.get("overhangs_bridge_threshold").is_enabled()) {
                                            detector.set_max_bridge_length(
                                                scale_d(std::max(0.,
                                                                 other_region_config.get("overhangs_bridge_threshold")
                                                                     .get_float())));
                                        } else {
                                            detector.set_max_bridge_length(-1);
                                        }
                                        if (detector.detect_angle()) {
                                            StoredPolygonCollection coverage_polygons(storage);
                                            if (detector.coverage(coverage_polygons) > 0) {
                                                new_bridged_area_co += clipper_union2(new_bridged_area_co,
                                                                                      clipper(coverage_polygons));
                                            }
                                        }
                                    });
                                }
                                // TODO: if overhangs_bridge_upper_layers goes
                                // from 2+ to 0, detect that we cannot go higher
                                // inside the region.
                            }
                            if (!new_bridged_area_co.empty()) {
                                bridged_other_layers_areas_co += new_bridged_area_co;
                                // Update the area still unsupported after the
                                // newly accepted bridgeable region.
                                still_unsupported_co = clipper_diff(still_unsupported_co, new_bridged_area_co);
                                still_unsupported_co = clipper_offset2(still_unsupported_co,
                                                                       double(-bridge_flow.spacing / 2),
                                                                       double(bridge_flow.spacing / 2));
                                // Make this bridge coverage available to higher
                                // look-ahead layers.
                                if (other_layer_bridge_idx + 1 < max_layer_idx) {
                                    previous_supported_co = clipper_union2(previous_supported_co, new_bridged_area_co);
                                }
                            }
                        }
                    }
                }
            }

            // Enlarge supported area and intersect it with this region's original
            // slices. This produces the final raw slices for the region.
            // TODO: fuse region with same enlargement
            coord_t enlargement = scale_i(
                region_config.get("overhangs_max_slope").get_effective_value(unscaled(max_nz_diam)));
            if (enlargement > 0) {
                ClipperOperand enlarged_support_co = clipper_offset(supported_area_co, double(enlargement));
                if (!bridged_other_layers_areas_co.empty()) {
                    bridged_other_layers_areas_co = clipper_union(bridged_other_layers_areas_co);
                    // Only remove bridge areas not fully enclosed by the enlarged
                    // support; enclosed bridge areas do not affect the boundary.
                    bridged_other_layers_areas_co.for_each_expolygon([&](const ExPolygon &bridged_other_layers_area) {
                        ClipperOperand check_co = clipper_diff(clipper(bridged_other_layers_area), enlarged_support_co);
                        if (!check_co.empty()) {
                            StoredExPolygonCollection check = check_co.to_expolygon_collection();
                            if (check.size() > 1 || !equals(check.at(0), bridged_other_layers_area)) {
                                enlarged_support_co = clipper_diff(enlarged_support_co,
                                                                   clipper(bridged_other_layers_area));
                            }
                        }
                    });
                }
                enlarged_support_co = clipper_union_with_safety_offset(enlarged_support_co);
                ClipperOperand max_enlarged_support_co = clipper_offset(enlarged_support_co,
                                                                        double(enlargement * 0.5));
                ClipperOperand min_enlarged_support_co = clipper_union2(supported_area_co,
                                                                        clipper_offset(enlarged_support_co,
                                                                                       double(-enlargement * 0.5)));

                // Put bridgeable areas back into the supported masks. Bridges are
                // preserved, not enlarged.
                max_enlarged_support_co = clipper_union2(max_enlarged_support_co, bridged_area_co);
                min_enlarged_support_co = clipper_union2(min_enlarged_support_co, bridged_area_co);

                // This materialized view is currently read-only. It keeps the
                // structure ready for re-enabling smoothing once mutable
                // collection-element views are available.
                StoredExPolygonCollection enlarged_support = enlarged_support_co.to_expolygon_collection();
                for (ExPolygon expoly : enlarged_support) {
                    assert(expoly.contour().is_counter_clockwise());
                    // FIXME: Geometry views are read-only now. Add an explicit StoredExPolygonCollection mutation
                    // helper before re-enabling smoothing of materialized Clipper results. same with holes (concave
                    // as they are in reverse order, this is taken care inside only_convex_or_90deg)
                    for (Polygon hole : expoly.holes()) {
                        assert(hole.is_clockwise());
                    }
                }
                enlarged_support_co = clipper_intersection(clipper(enlarged_support), max_enlarged_support_co);
                enlarged_support_co = clipper_union2(enlarged_support_co, min_enlarged_support_co);
                // Build replacement raw slices for this LayerRegion by clipping
                // the enlarged support against each original raw slice.
                StoredExPolygonCollection new_slices(storage);
                ExPolygonCollection src_slices = lregion.slices();
                for (size_t slice_idx = 0; slice_idx < src_slices.size(); slice_idx++) {
                    ClipperOperand new_slice_co = clipper_intersection(enlarged_support_co,
                                                                       clipper(src_slices[slice_idx]));
                    // If look-ahead bridges were found, smooth the enlargement so
                    // spikes do not appear near bridge boundaries.
                    if (!bridged_other_layers_areas_co.empty()) {
                        new_slice_co = clipper_offset2(new_slice_co, double(-enlargement / 2),
                                                       double(enlargement / 2));
                    }
                    if (!new_slice_co.empty()) {
                        new_slices.append_move_from(new_slice_co.to_expolygon_collection());
                    }
                }
                // TODO: ensure_valid(new_slices, resolution).
                // Update raw_slices. Surfaces are intentionally left for later
                // pipeline steps to regenerate from these raw slices.
                layer_has_changed = true;
                layer_handle *mut_layer = ctx->object_borrow_mutable_layer(ctx->object, layer_idx);
                layer_region_handle *mut_lregion = layer_get_region_mutable(mut_layer, lregion_idx);
                expolygon_collection_handle *mut_region_slices = ctx->layer_region_borrow_mutable_slices(mut_lregion);
                expolygons_move(mut_region_slices, new_slices.mutable_handle());
            }
        }
        // Raw LayerRegion slices changed, so rebuild the layer-level slice cache
        // and islands once after all regions for this layer are processed.
        if (layer_has_changed) {
            layer_handle *mut_layer = ctx->object_borrow_mutable_layer(ctx->object, layer_idx);
            ctx->layer_recompute_slices_and_islands_from_layer_region(mut_layer);
        }
        progress().increment();
    }
    progress().finish_run();

    assert(storage_size(storage) == 0);
    storage_clear(storage);
}

MaxOverhangThreshold &MaxOverhangThreshold::instance(orchestrator_handle *orch) {
    static MaxOverhangThreshold s_instance(orch);
    return s_instance;
}

const char *MaxOverhangThreshold::id_impl() const noexcept { return k_max_overhang_threshold_id; }

const char *MaxOverhangThreshold::name_impl() const noexcept { return "Max overhang threshold"; }

const char *MaxOverhangThreshold::description_impl() const noexcept
{
    return "Cut unsupported bridge spans from overhang detection using plugin-defined settings.";
}

slicing_step_t MaxOverhangThreshold::step_impl() const noexcept { return STEP_POST_SLICING; }

const char *const *MaxOverhangThreshold::dependencies_impl() const noexcept { return k_no_dependencies; }

int32_t MaxOverhangThreshold::priority_impl() const noexcept { return 0; }

int32_t MaxOverhangThreshold::used_config_keys(raw_used_config_key *keys) const noexcept
{
    if (keys != nullptr)
        std::copy(std::begin(k_used_config_keys), std::end(k_used_config_keys), keys);
    return int32_t(std::size(k_used_config_keys));
}

int32_t MaxOverhangThreshold::defined_config_keys(const char **keys) const noexcept
{
    if (keys != nullptr)
        for (size_t idx = 0; idx < k_defined_config_key_count; ++idx)
            keys[idx] = k_used_config_keys[idx].key;
    return int32_t(k_defined_config_key_count);
}

const char *MaxOverhangThreshold::progress_message_format_impl() const noexcept
{
    return "Max overhang threshold: %u / %u layers";
}

const char *MaxOverhangThreshold::print_ui_fragment() noexcept
{
    return "page:Slicing\n"
           "group:Modifying slices\n"
           "line:Overhangs cut\n"
           "setting:overhangs_max_slope\n"
           "setting:overhangs_bridge_threshold\n"
           "setting:overhangs_bridge_upper_layers\n"
           "end_line\n";
}

void MaxOverhangThreshold::inilialize_impl(storage_handle *) const
{
    raw_config_option_def def = raw_config_option_def_init();
    def.opt_key = "overhangs_bridge_threshold";
    def.type = RAW_CO_FLOAT;
    def.container_type = RAW_CONTAINER_TYPE_REGION;
    def.option_preset_type = RAW_PRESET_TYPE_FFF_PRINT;
    def.printer_technology = RAW_PT_FFF;
    def.label = "Bridge max length";
    def.category = RAW_OPTION_CATEGORY_SLICING;
    def.invalidates_step = STEP_SLICING;
    def.tooltip = ("Maximum distance for bridges. If the distance is over that, it will be considered as overhangs for 'overhangs_max_slope'."
                   "\nIf disabled, accept all distances."
                   "\nSet to 0 to ignore bridges.");
    def.sidetext = "mm";
    def.has_min = true;
    def.min_value = 0;
    def.can_be_disabled = true;
    def.mode = RAW_CONFIG_OPTION_MODE_EXPERT | RAW_CONFIG_OPTION_MODE_SUSI;
    def.default_serialized_value = "!0";
    orchestrator_create_option_def(m_orchestrator, &def);

    def = raw_config_option_def_init();
    def.opt_key = "overhangs_bridge_upper_layers";
    def.type = RAW_CO_INT;
    def.container_type = RAW_CONTAINER_TYPE_REGION;
    def.option_preset_type = RAW_PRESET_TYPE_FFF_PRINT;
    def.printer_technology = RAW_PT_FFF;
    def.label = "Consider upper bridges";
    def.category = RAW_OPTION_CATEGORY_SLICING;
    def.invalidates_step = STEP_SLICING;
    def.tooltip = ("Don't put overhangs in the area if it will be filled in next layer(s) by bridges."
                   "\nIf set to 0, it will look all layers."
                   "\nIf disabled, the current layer will still add overhangs, even if there's a bridge on top, reducing the bridge length.");
    def.sidetext = "layers";
    def.has_min = true;
    def.min_value = 0;
    def.can_be_disabled = true;
    def.mode = RAW_CONFIG_OPTION_MODE_EXPERT | RAW_CONFIG_OPTION_MODE_SUSI;
    def.default_serialized_value = "2";
    orchestrator_create_option_def(m_orchestrator, &def);

    def = raw_config_option_def_init();
    def.opt_key = "overhangs_max_slope";
    def.type = RAW_CO_FLOAT_OR_PERCENT;
    def.container_type = RAW_CONTAINER_TYPE_REGION;
    def.option_preset_type = RAW_PRESET_TYPE_FFF_PRINT;
    def.printer_technology = RAW_PT_FFF;
    def.label = "Overhangs max slope";
    def.full_label = "Overhangs max slope";
    def.category = RAW_OPTION_CATEGORY_SLICING;
    def.invalidates_step = STEP_SLICING;
    def.tooltip = ("Maximum slope for overhangs. if at each layer, the overhangs hangs by more than this value, then the geometry will be cut."
                   " It doesn't cut into detected bridgeable areas if 'overhangs_bridge_threshold' allow it."
                   "\nCan be a % of the highest nozzle diameter."
                   "\nSet to 0 to disable.");
    def.sidetext = "mm or %";
    def.ratio_over = "nozzle_diameter";
    def.has_min = true;
    def.min_value = 0;
    def.mode = RAW_CONFIG_OPTION_MODE_EXPERT | RAW_CONFIG_OPTION_MODE_SUSI;
    def.default_serialized_value = "0";
    orchestrator_create_option_def(m_orchestrator, &def);

    orchestrator_add_ui_fragment(m_orchestrator,
                                 "print.ui",
                                 k_max_overhang_threshold_id,
                                 MaxOverhangThreshold::print_ui_fragment(),
                                 1);

    raw_gui_rule rule = raw_gui_rule_init();
    rule.action = RAW_GUI_RULE_ACTION_ENABLE;
    rule.condition = RAW_GUI_RULE_CONDITION_VALUE_NON_ZERO;
    rule.condition_key = "overhangs_max_slope";
    rule.target_key = "overhangs_bridge_threshold";
    orchestrator_add_gui_rule(m_orchestrator, &rule);
    rule.target_key = "overhangs_bridge_upper_layers";
    orchestrator_add_gui_rule(m_orchestrator, &rule);
}

void MaxOverhangThreshold::setup_run_impl(const plugin_run_context *run_ctx) const
{
    const run_ctx_post_slicing *ctx = plugin_ctx_as_post_slicing(run_ctx);
    if (ctx == nullptr || ctx->object == nullptr)
        return;

    progress().add_max(layer_work_count(Object(ctx->object)));
}

void register_max_overhang_threshold_plugin(orchestrator_handle *orch) {
    orchestrator_register_plugin(orch, MaxOverhangThreshold::instance(orch).c_instance());
}

}} // namespace slic3r_api::MaxOverhangThresholdPlugin

#ifdef MAX_OVERHANG_THRESHOLD_PLUGIN_DLL
extern "C" void register_plugin(orchestrator_handle *orch) {
    slic3r_api::MaxOverhangThresholdPlugin::register_max_overhang_threshold_plugin(orch);
}
#endif // MAX_OVERHANG_THRESHOLD_PLUGIN_DLL
