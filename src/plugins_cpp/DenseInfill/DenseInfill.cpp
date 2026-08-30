///|/ Copyright (c) SuperSlicer 2026 Durand Remi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/

#include "DenseInfill.hpp"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "libslic3r/Api/plugin/c/slic3r_orchestrator.h"
#include "libslic3r/Api/plugin/c/steps/slic3r_step_infill.h"
#include "libslic3r/Api/plugin/c/steps/slic3r_step_post_infill.h"
#include "libslic3r/Api/plugin/c/steps/slic3r_step_surface_generation.h"
#include "libslic3r/Api/plugin/cpp/ClipperViews.hpp"
#include "libslic3r/Api/plugin/cpp/DataTreeViews.hpp"
#include "libslic3r/Api/plugin/cpp/ExtrusionViews.hpp"
#include "libslic3r/Api/plugin/cpp/ParallelFor.hpp"
#include "libslic3r/Api/plugin/cpp/RegionSettingsViews.hpp"

namespace slic3r_api { namespace DenseInfillPlugin {
namespace {

const char *k_surface_marker_id = "dense_infill.surface_marker";
const char *k_recipe_modifier_id = "dense_infill.recipe_modifier";
const char *k_post_infill_order_id = "dense_infill.post_infill_order";

const char *k_no_dependencies[] = { nullptr };

const char *k_infill_dense_key = "infill_dense";
const char *k_infill_dense_algo_key = "infill_dense_algo";
const char *k_fill_density_key = "fill_density";
const char *k_external_infill_margin_key = "external_infill_margin";
const char *k_infill_extruder_key = "infill_extruder";
const char *k_solid_infill_extruder_key = "solid_infill_extruder";
const char *k_perimeters_key = "perimeters";
const char *k_nozzle_diameter_key = "nozzle_diameter";
const char *k_resolution_key = "resolution";

const raw_used_config_key k_surface_used_config_keys[] = {
    { k_infill_dense_key, RAW_CO_BOOL, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE },
    { k_infill_dense_algo_key, RAW_CO_ENUM, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE },
    { k_fill_density_key, RAW_CO_PERCENT, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE },
    { k_external_infill_margin_key, RAW_CO_FLOAT_OR_PERCENT, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE },
    { k_infill_extruder_key, RAW_CO_INT, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE },
    { k_solid_infill_extruder_key, RAW_CO_INT, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE },
    { k_perimeters_key, RAW_CO_INT, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE },
    { k_nozzle_diameter_key, RAW_CO_VECTOR_FLOAT, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE },
    { k_resolution_key, RAW_CO_FLOAT, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE }
};

constexpr raw_surface_type k_dense_surface_type =
    RAW_SURFACE_TYPE_POS_INTERNAL | RAW_SURFACE_TYPE_DENS_SPARSE | RAW_SURFACE_TYPE_MOD_BRIDGE;

enum class DenseAlgo
{
    Automatic,
    AutoNotFull,
    AutoOrEnlarged,
    AutoOrNothing,
    Enlarged,
    Disabled
};

StoredExPolygonCollection dense_fill_fit_to_size(storage_handle *storage,
                                                 const ExPolygon &bad_polygon_to_cover,
                                                 const ExPolygon &growing_area,
                                                 const coord_t offset,
                                                 float coverage)
{
    ClipperContext clipper(storage);

    const auto try_fit_to_size = [storage](const ExPolygon &polygon_to_check, const ExPolygon &allowed_points) {
        StoredExPolygon polygon_reduced(storage);
        polygon_reduced.copy_from(polygon_to_check);

        polygon_handle *contour = expolygon_contour(polygon_reduced.mutable_handle());
        multipoint_handle *contour_points = polygon_as_multipoint(contour);
        uint32_t pos_check = 0;
        while (pos_check < multipoint_size(contour_points)) {
            const c_point tested_point = multipoint_get(contour_points, pos_check);
            c_point best_point = polygon_point_projection(allowed_points.contour().handle(), tested_point, nullptr);
            for (uint32_t hole_idx = 0; hole_idx < allowed_points.hole_size(); ++hole_idx) {
                const c_point hole_point = polygon_point_projection(allowed_points.hole(hole_idx).handle(), tested_point, nullptr);
                if (norm(hole_point - tested_point) < norm(best_point - tested_point))
                    best_point = hole_point;
            }
            if (norm(best_point - tested_point) < scale_i(0.01))
                ++pos_check;
            else
                multipoint_erase(contour_points, pos_check, 1);
        }
        // edge case
        if (multipoint_size(contour_points) == 1)
            multipoint_clear(contour_points);
        polygon_reduced.holes_clear();
        return polygon_reduced;
    };

    //fix uncoverable area
    StoredExPolygonCollection polygons_to_cover =
        clipper_intersection(clipper(bad_polygon_to_cover), clipper(growing_area)).to_expolygon_collection();
    if (polygons_to_cover.size() != 1)
        return StoredExPolygonCollection(storage, growing_area);
    const ExPolygon polygon_to_cover = polygons_to_cover.front();

    //grow the polygon_to_check enough to cover polygon_to_cover
    float current_coverage = coverage;
    coord_t previous_offset = 0;
    coord_t current_offset = offset;
    StoredExPolygon polygon_reduced = try_fit_to_size(polygon_to_cover, growing_area);
    while (polygon_reduced.contour().size() < 3) {
        current_offset *= 2;
        StoredExPolygonCollection bigger_polygon =
            clipper_offset(clipper(polygon_to_cover), double(current_offset)).to_expolygon_collection();
        if (bigger_polygon.size() != 1)
            break;
        bigger_polygon =
            clipper_intersection(clipper(bigger_polygon.front()), clipper(growing_area)).to_expolygon_collection();
        if (bigger_polygon.size() != 1)
            break;
        polygon_reduced = try_fit_to_size(bigger_polygon.front(), growing_area);
    }
    //ExPolygons to_check = offset_ex(polygon_to_cover, -offset);
    StoredExPolygonCollection not_covered =
        clipper_diff_with_safety_offset(clipper(polygon_to_cover), clipper(polygon_reduced.readonly())).to_expolygon_collection();
    while (!not_covered.empty()) {
        //not enough, use a bigger offset
        float percent_coverage = float(polygon_reduced.area() / growing_area.area());
        float next_coverage = percent_coverage + (percent_coverage - current_coverage) * 4;
        previous_offset = current_offset;
        current_offset *= 2;
        if (next_coverage < 0.1f)
            current_offset *= 2;
        //create the bigger polygon and test it
        StoredExPolygonCollection bigger_polygon =
            clipper_offset(clipper(polygon_to_cover), double(current_offset)).to_expolygon_collection();
        if (bigger_polygon.size() != 1) {
            // Error, growing a single polygon result in many/no other  => abord
            return StoredExPolygonCollection(storage);
        }
        bigger_polygon =
            clipper_intersection(clipper(bigger_polygon.front()), clipper(growing_area)).to_expolygon_collection();
        // After he intersection, we may have section of the bigger_polygon that jumped over a 'clif' to exist in an other area, have to remove them.
        if (bigger_polygon.size() > 1) {
            //remove polygon not in intersection with polygon_to_cover
            for (uint32_t i = 0; i < bigger_polygon.size();) {
                StoredExPolygonCollection contact =
                    clipper_intersection(clipper(bigger_polygon[i]), clipper(polygon_to_cover)).to_expolygon_collection();
                if (contact.empty())
                    bigger_polygon.erase(i);
                else
                    ++i;
            }
        }
        if (bigger_polygon.size() != 1 || bigger_polygon.front().area() > growing_area.area()) {
            // Growing too much  => we can as well use the full coverage, in this case
            polygon_reduced.copy_from(growing_area);
            break;
            //return ExPolygons() = { growing_area };
        }
        //polygon_reduced = try_fit_to_size(bigger_polygon[0], allowedPoints);
        polygon_reduced = try_fit_to_size(bigger_polygon.front(), growing_area);
        not_covered =
            clipper_diff_with_safety_offset(clipper(polygon_to_cover), clipper(polygon_reduced.readonly())).to_expolygon_collection();
    }
    //ok, we have a good one, now try to optimise (unless there are almost no growth)
    if (current_offset > offset * 3) {
        //try to shrink
        uint32_t nb_opti_max = 6;
        for (uint32_t i = 0; i < nb_opti_max; ++i) {
            coord_t new_offset = (previous_offset + current_offset) / 2;
            StoredExPolygonCollection bigger_polygon =
                clipper_offset(clipper(polygon_to_cover), double(new_offset)).to_expolygon_collection();
            if (bigger_polygon.size() != 1) {
                //Warn, growing a single polygon result in many/no other, use previous good result
                break;
            }
            bigger_polygon =
                clipper_intersection(clipper(bigger_polygon.front()), clipper(growing_area)).to_expolygon_collection();
            if (bigger_polygon.size() != 1 || bigger_polygon.front().area() > growing_area.area()) {
                //growing too much, use previous good result (imo, should not be possible to enter this branch)
                break;
            }
            //ExPolygon polygon_test = try_fit_to_size(bigger_polygon[0], allowedPoints);
            StoredExPolygon polygon_test = try_fit_to_size(bigger_polygon.front(), growing_area);
            not_covered =
                clipper_diff_with_safety_offset(clipper(polygon_to_cover), clipper(polygon_test.readonly())).to_expolygon_collection();
            if (!not_covered.empty()) {
                //bad, not enough, use a bigger offset
                previous_offset = new_offset;
            } else {
                //good, we may now try a smaller offset
                current_offset = new_offset;
                polygon_reduced = std::move(polygon_test);
            }
        }
    }

    //return the area which cover the growing_area. Intersect it to retreive the holes.
    StoredExPolygonCollection to_print =
        clipper_intersection(clipper(polygon_reduced.readonly()), clipper(growing_area)).to_expolygon_collection();

    //remove polygon not in intersection with polygon_to_cover
    for (uint32_t i = 0; i < to_print.size();) {
        StoredExPolygonCollection contact =
            clipper_intersection(clipper(to_print[i]), clipper(polygon_to_cover)).to_expolygon_collection();
        if (contact.empty())
            to_print.erase(i);
        else
            ++i;
    }
    return to_print;
}

struct DenseSurfaceMarkerResult
{
    const layer_region_island_handle *region_island = nullptr;
    StoredSurfaceCollection surfaces;

    DenseSurfaceMarkerResult(const layer_region_island_handle *region_island, StoredSurfaceCollection &&surfaces) :
        region_island(region_island), surfaces(std::move(surfaces)) {}
    DenseSurfaceMarkerResult(DenseSurfaceMarkerResult &&) noexcept = default;
    DenseSurfaceMarkerResult &operator=(DenseSurfaceMarkerResult &&) noexcept = default;
};

struct DenseSurfaceMarkerStore
{
    const run_ctx_surface_generation *ctx = nullptr;
    const PluginPropertyKey<SurfaceDenseInfillHint> *hint_property = nullptr;
    storage_handle *persistent_storage = nullptr;
    std::mutex mutex;
    std::vector<DenseSurfaceMarkerResult> results;
};

void process_surface_marker_surface(storage_handle *storage,
                                    const PluginPropertyKey<SurfaceDenseInfillHint> &hint_property,
                                    const Print &print,
                                    const LayerIsland &island,
                                    const LayerRegionIsland &region_island,
                                    const RegionSettings &settings,
                                    const Surface &surface,
                                    StoredSurfaceCollection &surface_output,
                                    bool &changed)
{
    const auto area_sum = [](const ExPolygonCollection &areas) {
        double out = 0.;
        for (const ExPolygon area : areas)
            out += std::abs(area.area());
        return out;
    };
    const auto dense_algo_from_serialized = [](std::string serialized) {
        if (!serialized.empty() && serialized.front() == '!')
            serialized.erase(serialized.begin());
        if (serialized == "autonotfull")
            return DenseAlgo::AutoNotFull;
        if (serialized == "autoenlarged")
            return DenseAlgo::AutoOrEnlarged;
        if (serialized == "autosmall")
            return DenseAlgo::AutoOrNothing;
        if (serialized == "enlarged")
            return DenseAlgo::Enlarged;
        if (serialized == "disabled")
            return DenseAlgo::Disabled;
        return DenseAlgo::Automatic;
    };
    const auto max_nozzle_diameter = [&print]() {
        if (!print.config().has(k_nozzle_diameter_key))
            return scale_d(0.4);
        const ConfigOption nozzles = print.config().get(k_nozzle_diameter_key);
        double max_nozzle = 0.;
        for (uint32_t idx = 0; idx < nozzles.size(); ++idx)
            max_nozzle = std::max(max_nozzle, nozzles.get_float(idx));
        return scale_d(max_nozzle > 0. ? max_nozzle : 0.4);
    };

    // Dense infill only refines sparse/void surfaces. Existing solid surfaces
    // are inputs for other layers to detect support, so this pass must preserve
    // them exactly instead of reclassifying them.
    if (surface_type_is_solid(surface.type())) {
        surface_output.append_like(surface.expolygon(), surface);
        return;
    }

    ClipperContext clipper(storage);
    bool surface_changed = false;

    // Split the source surface by the settings that control dense infill. Each
    // clipped piece is handled independently, which keeps region-specific
    // settings local while still writing the final surfaces into the
    // LayerRegionIsland-owned output collection.
    for (const auto &[settings_value, settings_clip] : settings.get_areas(k_infill_dense_key)) {
        StoredExPolygonCollection source_parts = settings_clip.intersections(surface.expolygon());
        if (source_parts.empty())
            continue;

        const double fill_density_value = settings_value.get_float(k_fill_density_key);
        const double fill_density_percent = fill_density_value <= 1.0 ? fill_density_value * 100.0 : fill_density_value;
        DenseAlgo setting_algo = dense_algo_from_serialized(settings_value.option(k_infill_dense_algo_key).serialize());
        if (!settings_value.get_bool(k_infill_dense_key) ||
            fill_density_percent >= 40. ||
            setting_algo == DenseAlgo::Disabled) {
            // This settings zone does not request dense infill. We still copy
            // the clipped part, because another settings zone may densify a
            // different part of the same original surface.
            surface_output.append_like_move(std::move(source_parts), surface);
            continue;
        }

        for (const ExPolygon surf_with_overlap : source_parts) {
            // sparse_polys is the remaining ordinary sparse area for this
            // settings piece. Each accepted dense patch is removed from it
            // immediately, so later upper surfaces cannot create overlapping
            // dense patches.
            StoredExPolygonCollection sparse_polys(storage, surf_with_overlap);
            StoredExPolygonCollection dense_polys(storage);
            std::vector<uint16_t> dense_priority;

            //find the surface which intersect with the smallest maxNb possible
            for (const LayerIsland upper_island : island.upper_islands()) {
                for (uint32_t upper_region_island_idx = 0; upper_region_island_idx < upper_island.region_island_count(); ++upper_region_island_idx) {
                    const LayerRegionIsland upper_region_island = upper_island.region_island(upper_region_island_idx);
                    for (const Surface upp : upper_region_island.fill_surfaces_collection()) {
                        if (!surface_type_is_solid(upp.type()) && !surface_type_is_bridge(upp.type()))
                            continue;

                        // i'm using intersection_ex because the result different than
                        // upp.expolygon.overlaps(surf.expolygon) or surf.expolygon.overlaps(upp.expolygon)
                        // and a little offset2 to remove the almost supported area
                        // LayerRegionIsland has already grouped compatible
                        // regions for this surface bucket. Dense infill only
                        // needs flow widths here, so the first region gives the
                        // same kind of values the legacy LayerRegion path used.
                        const LayerRegion layer_region = region_island.region(0);
                        const c_flow infill_flow = layer_region.flow(RAW_EXTRUSION_ROLE_INTERNAL_INFILL);
                        const coord_t scaled_width = infill_flow.width;
                        StoredExPolygonCollection intersect =
                            clipper_offset2(
                                clipper_intersection_with_safety_offset(clipper(sparse_polys.readonly()), clipper(upp.expolygon())),
                                -double(scaled_width),
                                double(scaled_width)).to_expolygon_collection();
                        if (!intersect.empty()) {
                            DenseAlgo algo = setting_algo;

                            //if no infill, don't bother, it's always yes
                            if (fill_density_value == 0.) {
                                if (algo == DenseAlgo::AutoOrEnlarged)
                                    algo = DenseAlgo::Automatic;
                                else if (algo != DenseAlgo::Automatic)
                                    algo = DenseAlgo::AutoNotFull;
                            }
                            if (algo == DenseAlgo::AutoOrNothing ||
                                algo == DenseAlgo::AutoOrEnlarged) {
                                //check if small enough
                                const double effective_density =
                                    std::max(0.0001, settings_value.get_effective_value(1., k_fill_density_key));
                                coordf_t min_width = max_nozzle_diameter() / effective_density;
                                StoredExPolygonCollection smalls =
                                    clipper_offset(clipper(intersect.readonly()), -min_width).to_expolygon_collection();
                                //small enough ?
                                if (smalls.empty()) {
                                    if (algo == DenseAlgo::AutoOrNothing)
                                        algo = DenseAlgo::AutoNotFull;
                                    if (algo == DenseAlgo::AutoOrEnlarged)
                                        algo = DenseAlgo::Automatic;
                                } else if (algo == DenseAlgo::AutoOrNothing) {
                                    algo = DenseAlgo::Disabled;
                                }
                            }
                            const int32_t perimeter_count = std::max(0, settings_value.get_int(k_perimeters_key));
                            const c_flow external_perimeter = layer_region.flow(RAW_EXTRUSION_ROLE_EXTERNAL_PERIMETER);
                            const c_flow perimeter = layer_region.flow(RAW_EXTRUSION_ROLE_INTERNAL_PERIMETER);
                            const double perimeter_width = perimeter_count == 0 ? 0. :
                                (unscaled(external_perimeter.width) + unscaled(perimeter.spacing) * double(perimeter_count - 1));
                            const double offset_expand =
                                settings_value.get_effective_value(perimeter_width, k_external_infill_margin_key);
                            if (algo == DenseAlgo::Enlarged) {
                                //expand the area a bit
                                intersect =
                                    clipper_offset(clipper(intersect.readonly()), scale_d(offset_expand)).to_expolygon_collection();
                                intersect =
                                    clipper_intersection(clipper(intersect.readonly()), clipper(sparse_polys.readonly())).to_expolygon_collection();
                            } else if (algo == DenseAlgo::Disabled) {
                                intersect.clear();
                            } else {
                                // Automatic modes try to cover the upper solid
                                // with a compact dense patch. AutoNotFull
                                // refuses to densify the whole sparse surface,
                                // while the other modes may enlarge or fit the
                                // patch depending on their legacy rules.
                                double sparse_area = surf_with_overlap.area();
                                double area_to_cover = 0;
                                if (algo == DenseAlgo::AutoNotFull) {
                                    // calculate area to decide if area is small enough for autofill
                                    area_to_cover = area_sum(intersect.readonly());
                                    // if we have to fill everything, don't bother
                                    if (area_to_cover * 1.1 > sparse_area)
                                        intersect.clear();
                                }
                                //like intersect.empty() but more resilient
                                StoredExPolygonCollection cover_intersect(storage);

                                // it will be a dense infill, split the surface if needed
                                //ExPolygons cover_intersect;
                                for (const ExPolygon expoly_tocover : intersect) {
                                    StoredExPolygonCollection temp =
                                        dense_fill_fit_to_size(
                                            storage,
                                            expoly_tocover,
                                            surf_with_overlap,
                                            4 * scaled_width,
                                            0.01f);
                                    cover_intersect.append_move_from(std::move(temp));
                                }
                                // calculate area to decide if area is small enough for autofill
                                if (algo == DenseAlgo::AutoOrEnlarged) {
                                    // Compare the legacy fitted shape with the
                                    // simply enlarged shape and keep the one
                                    // that covers less area. This limits the
                                    // amount of 50% dense infill we introduce.
                                    double area_dense_covered = area_sum(cover_intersect.readonly());
                                    // if enlarge is smaller, use enlarge
                                    intersect =
                                        clipper_offset(clipper(intersect.readonly()), scale_d(offset_expand)).to_expolygon_collection();
                                    intersect =
                                        clipper_intersection(clipper(intersect.readonly()), clipper(sparse_polys.readonly())).to_expolygon_collection();
                                    double area_enlarged_covered = area_sum(intersect.readonly());
                                    if (area_dense_covered < area_enlarged_covered)
                                        intersect = std::move(cover_intersect);
                                } else {
                                    intersect = std::move(cover_intersect);
                                }
                            }
                            if (!intersect.empty()) {

                                StoredExPolygonCollection sparse_surfaces =
                                    clipper_diff_with_safety_offset(clipper(sparse_polys.readonly()), clipper(intersect.readonly())).to_expolygon_collection();
                                StoredExPolygonCollection dense_surfaces =
                                    clipper_diff_with_safety_offset(clipper(sparse_polys.readonly()), clipper(sparse_surfaces.readonly())).to_expolygon_collection();
                                (void)dense_surfaces;
                                // Dense patches that overlap earlier dense
                                // patches must be printed later. The priority
                                // is stored on the resulting Surface and used
                                // by the post-infill ordering plugin.
                                for (const ExPolygon poly : intersect) {
                                    uint16_t priority = 1;
                                    StoredExPolygonCollection dense(storage, poly);
                                    for (uint32_t idx_dense = 0; idx_dense < dense_polys.size(); ++idx_dense) {
                                        const double area_before = area_sum(dense.readonly());
                                        StoredExPolygonCollection dense_test =
                                            clipper_diff_with_safety_offset(clipper(dense.readonly()), clipper(dense_polys[idx_dense])).to_expolygon_collection();
                                        if (area_sum(dense_test.readonly()) + double(SCALED_EPSILON) < area_before)
                                            priority = std::max(priority, uint16_t(dense_priority[idx_dense] + 1));
                                        dense = std::move(dense_test);
                                    }
                                    dense_polys.append_move_from(std::move(dense));
                                    while (dense_priority.size() < dense_polys.size())
                                        dense_priority.push_back(priority);
                                }
                                //assign (copy)
                                sparse_polys = std::move(sparse_surfaces);

                            }
                        }
                        //check if we are full-dense
                        if (sparse_polys.empty())
                            break;
                    }
                    if (sparse_polys.empty())
                        break;
                }
                if (sparse_polys.empty())
                    break;
            }

            //check if we need to split the surface
            if (!dense_polys.empty()) {
                double area_dense = area_sum(dense_polys.readonly());
                double area_sparse = area_sum(sparse_polys.readonly());
                // if almost no empty space, simplify by filling everything (else)
                if (area_sparse > area_dense * 0.1) {
                    //split
                    // Split mode: create dense surfaces for the dense patches,
                    // then put the leftover sparse area back as surfaces like
                    // the original. This keeps the island coverage intact.
                    for (uint32_t idx_dense = 0; idx_dense < dense_polys.size(); ++idx_dense) {
                        ExPolygon dense_poly = dense_polys[idx_dense];
                        //remove overlap with perimeter
                        StoredExPolygonCollection offseted_dense_polys =
                            island.infill_no_overlap_areas().empty() ?
                            StoredExPolygonCollection(storage, dense_poly) :
                            clipper_intersection(clipper(dense_poly), clipper(island.infill_no_overlap_areas())).to_expolygon_collection();
                        //add overlap with everything
                        const LayerRegion layer_region = region_island.region(0);
                        coord_t overlap = layer_region.flow(RAW_EXTRUSION_ROLE_INTERNAL_INFILL).width / 4;
                        offseted_dense_polys =
                            clipper_offset(clipper(offseted_dense_polys.readonly()), double(overlap)).to_expolygon_collection();
                        const coord_t scaled_resolution = print.config().has(k_resolution_key) ?
                            std::max(SCALED_EPSILON, scale_i(print.config().get(k_resolution_key).get_float())) :
                            SCALED_EPSILON;
                        offseted_dense_polys.ensure_valid(scaled_resolution);
                        const uint32_t first_new_idx = surface_output.size();
                        surface_output.append_move(std::move(offseted_dense_polys), k_dense_surface_type);
                        for (uint32_t idx = first_new_idx; idx < surface_output.size(); ++idx) {
                            MutableSurface dense_surface = surface_output.mutable_at(idx);
                            dense_surface.copy_properties_from(surface);
                            SurfaceDenseInfillHint &hint =
                                hint_property.get_or_add(dense_surface.mutable_properties());
                            hint.max_solid_layers_on_top = 1;
                            hint.priority = idx_dense < dense_priority.size() ? dense_priority[idx_dense] : 1;
                        }
                    }
                    sparse_polys = clipper_union(clipper(sparse_polys.readonly())).to_expolygon_collection();
                    const coord_t scaled_resolution = print.config().has(k_resolution_key) ?
                        std::max(SCALED_EPSILON, scale_i(print.config().get(k_resolution_key).get_float())) :
                        SCALED_EPSILON;
                    sparse_polys.ensure_valid(scaled_resolution);
                    surface_output.append_like_move(std::move(sparse_polys), surface);
                    surface_changed = true;
                } else {
                    // Almost all of the current piece became dense. Avoid tiny
                    // sparse leftovers and retype the complete piece as dense,
                    // which matches the legacy simplification branch.
                    const uint32_t first_new_idx = surface_output.size();
                    surface_output.append(surf_with_overlap, k_dense_surface_type);
                    MutableSurface dense_surface = surface_output.mutable_at(first_new_idx);
                    dense_surface.copy_properties_from(surface);
                    SurfaceDenseInfillHint &hint =
                        hint_property.get_or_add(dense_surface.mutable_properties());
                    hint.max_solid_layers_on_top = 1;
                    hint.priority = 1;
                    surface_changed = true;
                    break;
                }
            } else {
                surface_output.append_like(surf_with_overlap, surface);
                // mitigation: if this piece cannot be made dense, keep it as
                // its original sparse surface and let the other pieces be
                // tested independently.
                continue;
            }
        }
    }

    // If no settings clip produced anything for this surface, preserve the
    // original geometry. The caller publishes the whole LayerRegionIsland only
    // when at least one surface actually changed.
    if (surface_output.empty())
        surface_output.append_like(surface.expolygon(), surface);
    else if (surface_changed)
        changed = true;
}

void process_surface_marker_layer(uint32_t layer_idx,
                                  storage_handle *scratch_storage,
                                  const run_ctx_surface_generation *ctx,
                                  DenseSurfaceMarkerStore *store)
{
    assert(ctx != nullptr);
    assert(store != nullptr);
    assert(store->hint_property != nullptr);
    assert(store->persistent_storage != nullptr);
    assert(ctx->print != nullptr);
    assert(ctx->object != nullptr);

    const Print print(ctx->print);
    const Object object(ctx->object);
    const Layer layer = object.layer(layer_idx);
    for (uint32_t island_idx = 0; island_idx < layer.island_count(); ++island_idx) {
        const LayerIsland island = layer.island(island_idx);
        if (island.upper_island_count() == 0)
            continue;

        for (uint32_t region_island_idx = 0; region_island_idx < island.region_island_count(); ++region_island_idx) {
            const LayerRegionIsland region_island = island.region_island(region_island_idx);
            bool changed = false;
            StoredSurfaceCollection new_surfaces(scratch_storage);
            const SurfaceCollection input = region_island.fill_surfaces_collection();
            if (!input.empty() && region_island.region_count() > 0) {

                RegionSettings settings(scratch_storage, island,
                                        {{k_infill_dense_key, k_infill_dense_algo_key, k_fill_density_key,
                                          k_external_infill_margin_key, k_perimeters_key}});
                settings.segregate(island.slice());

                // check all surfaces to cover
                for (const Surface surface : input)
                    process_surface_marker_surface(scratch_storage, *store->hint_property, print, island, region_island, settings,
                                                   surface, new_surfaces, changed);
            }
            if (!changed)
                continue;

            // The per-job scratch storage is destroyed when this callback
            // returns. Copy the finished result into plugin storage under a
            // lock, then publish it later from the caller thread. worker threads compute proposed
            // replacements, but they never replace host surfaces while another
            // worker may still read them as "upper solid" input.
            std::lock_guard<std::mutex> lock(store->mutex);
            StoredSurfaceCollection persistent(store->persistent_storage);
            for (const Surface surface : new_surfaces.readonly()) {
                if (surface.expolygon().contour().size() >= 3)
                    persistent.append_like(surface.expolygon(), surface);
            }
            store->results.emplace_back(region_island.handle(), std::move(persistent));
        }
    }
}

void run_surface_marker_monothread(const plugin_run_context *run_ctx,
                                   const run_ctx_surface_generation *ctx,
                                   const PluginPropertyKey<SurfaceDenseInfillHint> &hint_property,
                                   PluginProgress &progress)
{
    if (ctx == nullptr || ctx->object == nullptr || ctx->print == nullptr ||
        ctx->set_region_island_fill_surfaces == nullptr)
        return;

    const Object object(ctx->object);
    DenseSurfaceMarkerStore store;
    store.ctx = ctx;
    store.hint_property = &hint_property;
    store.persistent_storage = run_ctx->plugin_storage;
    parallel_for_storage_with_progress(
        0,
        object.layer_count(),
        run_ctx,
        &progress,
        process_surface_marker_layer,
        ctx,
        &store);

    // now set the new surfaces
    for (DenseSurfaceMarkerResult &result : store.results) {
        ctx->set_region_island_fill_surfaces(
            const_cast<layer_region_island_handle *>(result.region_island),
            result.surfaces.mutable_handle());
    }
}

std::map<uint64_t, uint16_t> dense_priorities_for_island(
    const LayerIsland &island,
    const PluginPropertyKey<SurfaceDenseInfillHint> &hint_property)
{
    std::map<uint64_t, uint16_t> out;

    for (uint32_t region_island_idx = 0; region_island_idx < island.region_island_count(); ++region_island_idx) {
        const LayerRegionIsland region_island = island.region_island(region_island_idx);
        for (const Surface surface : region_island.fill_surfaces_collection()) {
            const SurfaceDenseInfillHint *hint = hint_property.get(surface.properties());
            if (hint != nullptr)
                out.emplace(surface.id(), hint->priority);
        }
    }
    return out;
}

struct DensePostInfillWork
{
    std::vector<const layer_region_handle *> destination_regions;
    std::map<uint16_t, std::vector<StoredExtrusionEntity>> dense_children_by_priority;
};

void append_destination_regions(DensePostInfillWork &work, const LayerRegionIsland &region_island)
{
    // The destination LayerRegionIsland must be compatible with every dense
    // subtree that will be moved into it. The step callback accepts a list of
    // LayerRegion handles, so this helper builds the union while keeping the
    // order deterministic for tests and debug output.
    for (const LayerRegion region : region_island.regions()) {
        const layer_region_handle *handle = region.handle();
        if (std::find(work.destination_regions.begin(), work.destination_regions.end(), handle) ==
            work.destination_regions.end())
            work.destination_regions.push_back(handle);
    }
}

int32_t destination_infill_extruder_id(const DensePostInfillWork &work)
{
    // The generic data-tree API receives an explicit extruder id. Dense infill
    // moves internal-infill subtrees, so the destination group must resolve to
    // one unique infill_extruder across every region it collected.
    int32_t destination_extruder_id = -1;
    for (const layer_region_handle *handle : work.destination_regions) {
        if (handle == nullptr)
            return -1;

        const LayerRegion region(handle);
        const Config config = region.print_region().config();
        if (!config.has(k_infill_extruder_key))
            return -1;

        const int32_t extruder_id = config.get(k_infill_extruder_key).get_int() - 1;
        if (extruder_id < 0)
            return -1;
        if (destination_extruder_id < 0) {
            destination_extruder_id = extruder_id;
            continue;
        }
        if (destination_extruder_id != extruder_id)
            return -1;
    }
    return destination_extruder_id;
}

bool extract_dense_children_from_root(storage_handle *storage,
                                      MutableExtrusionEntity root,
                                      const std::map<uint64_t, uint16_t> &priorities,
                                      DensePostInfillWork &work)
{
    if (priorities.empty() || root.child_count() == 0)
        return false;

    std::vector<uint32_t> normal_children;
    std::map<uint16_t, std::vector<uint32_t>> dense_indices_by_priority;
    for (uint32_t child_idx = 0; child_idx < root.child_count(); ++child_idx) {
        const ExtrusionEntity child = root.child(child_idx);
        const EPropertyInfill *infill = child.property<EPropertyInfill>();
        if (infill == nullptr) {
            normal_children.push_back(child_idx);
            continue;
        }

        const std::map<uint64_t, uint16_t>::const_iterator found =
            priorities.find(infill->source_surface_id);
        if (found == priorities.end())
            normal_children.push_back(child_idx);
        else
            dense_indices_by_priority[found->second].push_back(child_idx);
    }

    if (dense_indices_by_priority.empty())
        return false;

    // Clone before clearing the host root. Child indices in the clone stay
    // stable, which lets us rebuild the source root with only normal infill
    // while copying dense subtrees into the cross-region priority buckets.
    StoredExtrusionEntity original(storage, root.readonly());
    for (const auto &[priority, child_indices] : dense_indices_by_priority)
        for (uint32_t child_idx : child_indices)
            work.dense_children_by_priority[priority].emplace_back(storage, original.child(child_idx));

    if (normal_children.empty()) {
        // clear_content() would leave the host bucket as a generic nop entity.
        // LayerRegionIsland extrusion buckets are expected to stay collection
        // objects even when empty, because cleanup asks the collection whether
        // it contains printable children. The temporary child forces the
        // children-vector representation, then remove_child() leaves that
        // vector empty without creating fake printable output.
        root.clear_content();
        StoredExtrusionEntity placeholder(storage);
        root.append_child_move(placeholder.mutable_view());
        root.remove_child(0);
    } else {
        root.clear_content();
        for (uint32_t child_idx : normal_children)
            root.append_child_copy(original.child(child_idx));
    }

    return true;
}

void publish_dense_children_by_priority(const run_ctx_post_infill_generation &ctx,
                                        storage_handle *storage,
                                        const LayerIsland &island,
                                        DensePostInfillWork &work)
{
    if (work.dense_children_by_priority.empty() ||
        ctx.get_region_island_mutable_extrusion == nullptr)
        return;

    const int32_t destination_extruder_id = destination_infill_extruder_id(work);
    if (destination_extruder_id < 0)
        return;

    layer_region_island_handle *destination_region_island =
        layer_island_get_or_create_region_island(
            const_cast<layer_island_handle *>(island.handle()),
            work.destination_regions.empty() ? nullptr : work.destination_regions.data(),
            static_cast<uint32_t>(work.destination_regions.size()),
            destination_extruder_id);
    if (destination_region_island == nullptr)
        return;

    extrusion_entity_handle *destination_root_handle =
        ctx.get_region_island_mutable_extrusion(
            destination_region_island,
            RAW_EXTRUSION_ROLE_INTERNAL_INFILL);
    if (destination_root_handle == nullptr)
        return;

    MutableExtrusionEntity destination_root(destination_root_handle);
    destination_root.disable_sort();

    // Each priority bucket is a forced sequence. The destination root may sort
    // unrelated normal infill before this plugin runs, but once dense buckets
    // are appended their internal order must stay exactly as the surface pass
    // computed it: lower priority first, then higher priority.
    for (const auto &[priority, dense_children] : work.dense_children_by_priority) {
        (void)priority;
        StoredExtrusionEntity group(storage);
        for (const StoredExtrusionEntity &child : dense_children)
            group.append_child_copy(child.readonly());
        group.disable_sort();
        group.disable_reverse();
        destination_root.append_child_move(group.mutable_view());
    }
}

void process_post_infill_layer(uint32_t layer_idx,
                               storage_handle *scratch_storage,
                               const run_ctx_post_infill_generation *ctx,
                               const PluginPropertyKey<SurfaceDenseInfillHint> *hint_property)
{
    assert(ctx != nullptr);
    assert(hint_property != nullptr);
    assert(ctx->object != nullptr);
    assert(ctx->get_region_island_mutable_extrusion != nullptr);

    const Object object(ctx->object);
    const Layer layer = object.layer(layer_idx);
    for (uint32_t island_idx = 0; island_idx < layer.island_count(); ++island_idx) {
        const LayerIsland island = layer.island(island_idx);
        const std::map<uint64_t, uint16_t> priorities =
            dense_priorities_for_island(island, *hint_property);
        if (priorities.empty())
            continue;

        DensePostInfillWork work;
        for (uint32_t region_island_idx = 0; region_island_idx < island.region_island_count(); ++region_island_idx) {
            const LayerRegionIsland region_island = island.region_island(region_island_idx);
            if (!region_island.has_extrusion(RAW_EXTRUSION_ROLE_INTERNAL_INFILL))
                continue;

            extrusion_entity_handle *root_handle = ctx->get_region_island_mutable_extrusion(
                region_island.handle(),
                RAW_EXTRUSION_ROLE_INTERNAL_INFILL);
            if (root_handle == nullptr)
                continue;

            if (extract_dense_children_from_root(
                    scratch_storage,
                    MutableExtrusionEntity(root_handle),
                    priorities,
                    work))
                append_destination_regions(work, region_island);
        }
        publish_dense_children_by_priority(*ctx, scratch_storage, island, work);
    }
}

} // namespace

DenseInfillSurfaceMarker &DenseInfillSurfaceMarker::instance(orchestrator_handle *orch)
{
    static DenseInfillSurfaceMarker plugin(orch);
    return plugin;
}

DenseInfillSurfaceMarker::DenseInfillSurfaceMarker(orchestrator_handle *orch) :
    PluginBase(orch),
    m_hint_property(PluginPropertyKey<SurfaceDenseInfillHint>::register_dynamic(
        orch, DENSE_INFILL_HINT_PROPERTY_NAME))
{
}

const char *DenseInfillSurfaceMarker::id_impl() const noexcept { return k_surface_marker_id; }
const char *DenseInfillSurfaceMarker::name_impl() const noexcept { return "Dense infill surface marker"; }
const char *DenseInfillSurfaceMarker::description_impl() const noexcept
{
    return "Splits sparse fill surfaces under upper solid areas and marks the dense pieces.";
}
slicing_step_t DenseInfillSurfaceMarker::step_impl() const noexcept { return STEP_SURFACE_GENERATION; }
const char *const *DenseInfillSurfaceMarker::dependencies_impl() const noexcept { return k_no_dependencies; }
int32_t DenseInfillSurfaceMarker::priority_impl() const noexcept { return 30; }
const char *DenseInfillSurfaceMarker::progress_message_format_impl() const noexcept
{
    return "Marking dense infill: %u / %u layers";
}

int32_t DenseInfillSurfaceMarker::used_config_keys(raw_used_config_key *keys) const noexcept
{
    if (keys != nullptr)
        for (uint32_t idx = 0; idx < sizeof(k_surface_used_config_keys) / sizeof(k_surface_used_config_keys[0]); ++idx)
            keys[idx] = k_surface_used_config_keys[idx];
    return int32_t(sizeof(k_surface_used_config_keys) / sizeof(k_surface_used_config_keys[0]));
}

void DenseInfillSurfaceMarker::setup_run_impl(const plugin_run_context *run_ctx) const
{
    const run_ctx_surface_generation *ctx = plugin_ctx_as_surface_generation(run_ctx);
    if (ctx == nullptr || ctx->object == nullptr)
        return;
    progress().add_max(Object(ctx->object).layer_count());
}

void DenseInfillSurfaceMarker::run_impl(const plugin_run_context *run_ctx) const
{
    const run_ctx_surface_generation *ctx = plugin_ctx_as_surface_generation(run_ctx);
    if (ctx == nullptr || ctx->object == nullptr || ctx->print == nullptr)
        return;

    run_surface_marker_monothread(run_ctx, ctx, m_hint_property, progress());
}

DenseInfillRecipeModifier &DenseInfillRecipeModifier::instance(orchestrator_handle *orch)
{
    static DenseInfillRecipeModifier plugin(orch);
    return plugin;
}

DenseInfillRecipeModifier::DenseInfillRecipeModifier(orchestrator_handle *orch) :
    PluginBase(orch),
    m_hint_property(PluginPropertyKey<SurfaceDenseInfillHint>::register_dynamic(
        orch, DENSE_INFILL_HINT_PROPERTY_NAME))
{
}

const char *DenseInfillRecipeModifier::id_impl() const noexcept { return k_recipe_modifier_id; }
const char *DenseInfillRecipeModifier::name_impl() const noexcept { return "Dense infill recipe modifier"; }
const char *DenseInfillRecipeModifier::description_impl() const noexcept
{
    return "Turns marked dense-infill surfaces into 50% sparse infill recipes.";
}
slicing_step_t DenseInfillRecipeModifier::step_impl() const noexcept { return INFILL_SURFACE_RECIPE_MODIFIER; }
const char *const *DenseInfillRecipeModifier::dependencies_impl() const noexcept { return k_no_dependencies; }
int32_t DenseInfillRecipeModifier::priority_impl() const noexcept { return 0; }

void DenseInfillRecipeModifier::run_impl(const plugin_run_context *run_ctx) const
{
    const run_ctx_infill_surface_recipe_modifier *ctx =
        plugin_ctx_as_infill_surface_recipe_modifier(run_ctx);
    if (ctx == nullptr || ctx->surface == nullptr || ctx->params == nullptr)
        return;

    const Surface surface(ctx->surface);
    const SurfaceDenseInfillHint *hint = m_hint_property.get(surface.properties());
    if (hint == nullptr)
        return;

    // Dense infill is not solid infill. It keeps the selected sparse pattern
    // but asks the pattern for a denser local recipe, just like the legacy
    // dense-under-bridge pass.
    ctx->params->density = 0.5f;
    ctx->params->priority = int32_t(hint->priority);
}

DenseInfillPostInfillOrder &DenseInfillPostInfillOrder::instance(orchestrator_handle *orch)
{
    static DenseInfillPostInfillOrder plugin(orch);
    return plugin;
}

DenseInfillPostInfillOrder::DenseInfillPostInfillOrder(orchestrator_handle *orch) :
    PluginBase(orch),
    m_hint_property(PluginPropertyKey<SurfaceDenseInfillHint>::register_dynamic(
        orch, DENSE_INFILL_HINT_PROPERTY_NAME))
{
}

const char *DenseInfillPostInfillOrder::id_impl() const noexcept { return k_post_infill_order_id; }
const char *DenseInfillPostInfillOrder::name_impl() const noexcept { return "Dense infill print order"; }
const char *DenseInfillPostInfillOrder::description_impl() const noexcept
{
    return "Groups generated dense infill by priority after normal infill generation.";
}
slicing_step_t DenseInfillPostInfillOrder::step_impl() const noexcept { return STEP_POST_INFILL; }
const char *const *DenseInfillPostInfillOrder::dependencies_impl() const noexcept { return k_no_dependencies; }
int32_t DenseInfillPostInfillOrder::priority_impl() const noexcept { return 10; }
const char *DenseInfillPostInfillOrder::progress_message_format_impl() const noexcept
{
    return "Ordering dense infill: %u / %u layers";
}

void DenseInfillPostInfillOrder::setup_run_impl(const plugin_run_context *run_ctx) const
{
    const run_ctx_post_infill_generation *ctx = plugin_ctx_as_post_infill_generation(run_ctx);
    if (ctx == nullptr || ctx->object == nullptr)
        return;
    progress().add_max(Object(ctx->object).layer_count());
}

void DenseInfillPostInfillOrder::run_impl(const plugin_run_context *run_ctx) const
{
    const run_ctx_post_infill_generation *ctx = plugin_ctx_as_post_infill_generation(run_ctx);
    if (ctx == nullptr || ctx->object == nullptr || ctx->get_region_island_mutable_extrusion == nullptr)
        return;

    const Object object(ctx->object);
    parallel_for_storage_with_progress(
        0,
        object.layer_count(),
        run_ctx,
        &progress(),
        process_post_infill_layer,
        ctx,
        &m_hint_property);
}

void register_dense_infill_plugins(orchestrator_handle *orch)
{
    orchestrator_register_plugin(orch, DenseInfillSurfaceMarker::instance(orch).c_instance());
    orchestrator_register_plugin(orch, DenseInfillRecipeModifier::instance(orch).c_instance());
    orchestrator_register_plugin(orch, DenseInfillPostInfillOrder::instance(orch).c_instance());
}

}} // namespace slic3r_api::DenseInfillPlugin

#ifdef DENSE_INFILL_PLUGIN_DLL
SLIC3R_PLUGIN_DECLARE_ABI_VERSION()

extern "C" SLIC3R_PLUGIN_API void register_plugin(orchestrator_handle *orch)
{
    slic3r_api::DenseInfillPlugin::register_dense_infill_plugins(orch);
}

#endif // DENSE_INFILL_PLUGIN_DLL
