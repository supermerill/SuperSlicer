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

struct DenseArea
{
    StoredExPolygonCollection areas;
    uint16_t priority = 1;
};

struct DenseAreaResult
{
    std::vector<DenseArea> dense_areas;
    StoredExPolygonCollection dense_union;
};

plugin_property_type register_dense_hint_property(orchestrator_handle *orch)
{
    if (SurfaceDenseInfillHint::property_type == PLUGIN_PROPERTY_TYPE_INVALID) {
        SurfaceDenseInfillHint::property_type = orchestrator_register_property(
            orch,
            "superslicer.dense_infill.surface_hint",
            sizeof(SurfaceDenseInfillHint),
            alignof(SurfaceDenseInfillHint));
    }
    assert(SurfaceDenseInfillHint::property_type != PLUGIN_PROPERTY_TYPE_INVALID);
    return SurfaceDenseInfillHint::property_type;
}

double area_sum(const ExPolygonCollection &areas)
{
    double out = 0.;
    for (ExPolygon area : areas)
        out += std::abs(area.area());
    return out;
}

StoredExPolygonCollection union_collection(storage_handle *storage, const ExPolygonCollection &areas)
{
    if (areas.empty())
        return StoredExPolygonCollection(storage);

    ClipperContext clipper(storage);
    return clipper_union(clipper(areas)).to_expolygon_collection();
}

StoredExPolygonCollection intersection_collection(storage_handle *storage,
                                                  const ExPolygonCollection &lhs,
                                                  const ExPolygonCollection &rhs)
{
    if (lhs.empty() || rhs.empty())
        return StoredExPolygonCollection(storage);

    ClipperContext clipper(storage);
    return clipper_intersection(clipper(lhs), clipper(rhs)).to_expolygon_collection();
}

StoredExPolygonCollection diff_collection(storage_handle *storage,
                                          const ExPolygonCollection &lhs,
                                          const ExPolygonCollection &rhs)
{
    if (lhs.empty())
        return StoredExPolygonCollection(storage);
    if (rhs.empty())
        return lhs.clone(storage);

    ClipperContext clipper(storage);
    return clipper_diff(clipper(lhs), clipper(rhs)).to_expolygon_collection();
}

StoredExPolygonCollection offset_collection(storage_handle *storage,
                                            const ExPolygonCollection &areas,
                                            double delta)
{
    if (areas.empty())
        return StoredExPolygonCollection(storage);

    ClipperContext clipper(storage);
    return clipper_offset(clipper(areas), delta).to_expolygon_collection();
}

StoredExPolygonCollection offset2_collection(storage_handle *storage,
                                             const ExPolygonCollection &areas,
                                             double first_delta,
                                             double second_delta)
{
    if (areas.empty())
        return StoredExPolygonCollection(storage);

    ClipperContext clipper(storage);
    return clipper_offset2(clipper(areas), first_delta, second_delta).to_expolygon_collection();
}

StoredExPolygonCollection intersect_area_with_clip(storage_handle *storage,
                                                   const ExPolygon &area,
                                                   const RegionSettingsClip &clip)
{
    StoredExPolygonCollection clipped = clip.intersections(area);
    if (clipped.empty())
        return clipped;
    return union_collection(storage, clipped.readonly());
}

DenseAlgo dense_algo_from_serialized(std::string serialized)
{
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
}

DenseAlgo effective_dense_algo(const RegionSettingsValue &settings)
{
    DenseAlgo algo = dense_algo_from_serialized(settings.option(k_infill_dense_algo_key).serialize());

    // The legacy option keeps dense infill useful for vase-like sparse-free
    // prints. With 0% infill, the algorithms that normally avoid turning a
    // whole sparse surface dense must be remapped to variants that can still
    // create a local support patch under a bridge.
    if (settings.get_float(k_fill_density_key) <= 0.) {
        if (algo == DenseAlgo::AutoOrEnlarged)
            algo = DenseAlgo::Automatic;
        else if (algo != DenseAlgo::Automatic)
            algo = DenseAlgo::AutoNotFull;
    }

    return algo;
}

double fill_density_percent(const RegionSettingsValue &settings)
{
    const double value = settings.get_float(k_fill_density_key);
    return value <= 1.0 ? value * 100.0 : value;
}

bool dense_processing_enabled(const RegionSettingsValue &settings)
{
    return settings.get_bool(k_infill_dense_key) &&
           fill_density_percent(settings) < 40. &&
           effective_dense_algo(settings) != DenseAlgo::Disabled;
}

coord_t maximum_nozzle_diameter(const Print &print)
{
    if (!print.config().has(k_nozzle_diameter_key))
        return scale_d(0.4);

    const ConfigOption nozzles = print.config().get(k_nozzle_diameter_key);
    double max_nozzle = 0.;
    for (uint32_t idx = 0; idx < nozzles.size(); ++idx)
        max_nozzle = std::max(max_nozzle, nozzles.get_float(idx));

    return scale_d(max_nozzle > 0. ? max_nozzle : 0.4);
}

coord_t effective_external_margin(const RegionSettingsValue &settings, const LayerRegion &primary_region)
{
    const int32_t perimeter_count = std::max(1, settings.get_int(k_perimeters_key));
    const c_flow external_perimeter = primary_region.flow(RAW_EXTRUSION_ROLE_EXTERNAL_PERIMETER);
    const c_flow internal_perimeter = primary_region.flow(RAW_EXTRUSION_ROLE_INTERNAL_PERIMETER);
    const coord_t reference_width =
        external_perimeter.width + coord_t(std::max(0, perimeter_count - 1)) * internal_perimeter.spacing;
    return scale_d(settings.get_effective_value(unscaled(reference_width), k_external_infill_margin_key));
}

bool candidate_is_small_enough_for_auto(const ExPolygonCollection &candidate,
                                        const ExPolygonCollection &source,
                                        coord_t max_nozzle,
                                        double fill_density)
{
    if (candidate.empty())
        return false;

    const double source_area = std::max(1.0, area_sum(source));
    const double candidate_area = area_sum(candidate);
    const double density_factor = std::max(1.0, fill_density);
    const double loose_width = std::max<double>(max_nozzle, 1.0) / density_factor;

    // This is a deliberately conservative approximation of the old
    // boundary-fit test. It accepts only local patches that are small compared
    // with the source sparse surface or thin enough to be treated like a
    // bridge-anchor strip.
    return candidate_area <= source_area * 0.35 ||
           candidate_area <= loose_width * loose_width * 100.0;
}

StoredExPolygonCollection build_enlarged_dense_area(storage_handle *storage,
                                                    const ExPolygonCollection &source,
                                                    const ExPolygonCollection &candidate,
                                                    coord_t external_margin)
{
    if (source.empty() || candidate.empty())
        return StoredExPolygonCollection(storage);

    StoredExPolygonCollection expanded = offset_collection(storage, candidate, double(external_margin));
    return intersection_collection(storage, source, expanded.readonly());
}

StoredExPolygonCollection build_automatic_dense_area(storage_handle *storage,
                                                     const ExPolygonCollection &source,
                                                     const ExPolygonCollection &candidate,
                                                     coord_t infill_width)
{
    if (source.empty() || candidate.empty())
        return StoredExPolygonCollection(storage);

    // The legacy dense infill pass tries to make a compact area that the
    // infill pattern can cover with straight anchored strokes. Reproducing its
    // whole fitting search would require host-only geometry helpers, so the
    // plugin version keeps the same contract in a simpler way: grow the bridge
    // demand a few line widths, then clip it back to the current sparse
    // surface. Later cleanup plugins may merge tiny residuals.
    StoredExPolygonCollection expanded = offset_collection(storage, candidate, double(4 * infill_width));
    return intersection_collection(storage, source, expanded.readonly());
}

StoredExPolygonCollection compute_dense_area_for_value(storage_handle *storage,
                                                       const Print &print,
                                                       const LayerRegion &primary_region,
                                                       const RegionSettingsValue &settings,
                                                       const ExPolygonCollection &source,
                                                       const ExPolygonCollection &upper_solid)
{
    if (!dense_processing_enabled(settings) || source.empty() || upper_solid.empty())
        return StoredExPolygonCollection(storage);

    const c_flow infill_flow = primary_region.flow(RAW_EXTRUSION_ROLE_INTERNAL_INFILL);
    const coord_t infill_width = std::max<coord_t>(1, infill_flow.width);

    StoredExPolygonCollection candidate =
        intersection_collection(storage, source, upper_solid);
    candidate = offset2_collection(storage, candidate.readonly(), -double(infill_width), double(infill_width));
    if (candidate.empty())
        return candidate;

    DenseAlgo algo = effective_dense_algo(settings);
    const coord_t external_margin = effective_external_margin(settings, primary_region);
    const coord_t max_nozzle = maximum_nozzle_diameter(print);

    if (algo == DenseAlgo::AutoOrNothing || algo == DenseAlgo::AutoOrEnlarged) {
        const bool small_enough = candidate_is_small_enough_for_auto(
            candidate.readonly(),
            source,
            max_nozzle,
            fill_density_percent(settings));

        if (algo == DenseAlgo::AutoOrNothing)
            algo = small_enough ? DenseAlgo::AutoNotFull : DenseAlgo::Disabled;
        else
            algo = small_enough ? DenseAlgo::Automatic : DenseAlgo::Enlarged;
    }

    if (algo == DenseAlgo::Disabled)
        return StoredExPolygonCollection(storage);

    if (algo == DenseAlgo::Enlarged)
        return build_enlarged_dense_area(storage, source, candidate.readonly(), external_margin);

    StoredExPolygonCollection automatic =
        build_automatic_dense_area(storage, source, candidate.readonly(), infill_width);

    if (algo == DenseAlgo::AutoNotFull &&
        area_sum(automatic.readonly()) * 1.1 > area_sum(source))
        return StoredExPolygonCollection(storage);

    if (algo == DenseAlgo::AutoOrEnlarged) {
        StoredExPolygonCollection enlarged =
            build_enlarged_dense_area(storage, source, candidate.readonly(), external_margin);
        return area_sum(enlarged.readonly()) < area_sum(automatic.readonly()) ?
            std::move(enlarged) :
            std::move(automatic);
    }

    return automatic;
}

StoredExPolygonCollection collect_upper_solid_areas(storage_handle *storage, const LayerIsland &island)
{
    StoredExPolygonCollection out(storage);

    // Dense infill supports solid/bridge material in the layer above. The
    // upper-island links are already clipped by actual island overlap, so this
    // loop stays local to nearby geometry instead of scanning the whole layer.
    for (const LayerIsland upper_island : island.upper_islands()) {
        for (uint32_t region_island_idx = 0; region_island_idx < upper_island.region_island_count(); ++region_island_idx) {
            const LayerRegionIsland upper_region_island = upper_island.region_island(region_island_idx);
            for (const Surface surface : upper_region_island.fill_surfaces_collection()) {
                if (surface_type_is_solid(surface.type()) || surface_type_is_bridge(surface.type()))
                    out.push_back(surface.expolygon());
            }
        }
    }

    return union_collection(storage, out.readonly());
}

DenseAreaResult build_dense_areas_for_surface(storage_handle *storage,
                                              const Print &print,
                                              const LayerIsland &island,
                                              const LayerRegion &primary_region,
                                              const Surface &source,
                                              const ExPolygonCollection &upper_solid)
{
    DenseAreaResult result { {}, StoredExPolygonCollection(storage) };

    if (!surface_type_is_sparse(source.type()) || surface_type_is_solid(source.type()) || upper_solid.empty())
        return result;

    RegionSettings settings(storage,
                            island,
                            {{ k_infill_dense_key,
                               k_infill_dense_algo_key,
                               k_fill_density_key,
                               k_external_infill_margin_key,
                               k_perimeters_key }});
    settings.segregate(island.slice());

    ClipperContext clipper(storage);
    uint16_t priority = 1;
    for (const auto &[settings_value, settings_clip] : settings.get_areas(k_infill_dense_key)) {
        StoredExPolygonCollection source_in_settings =
            intersect_area_with_clip(storage, source.expolygon(), settings_clip);
        if (source_in_settings.empty())
            continue;

        StoredExPolygonCollection dense_area = compute_dense_area_for_value(
            storage,
            print,
            primary_region,
            settings_value,
            source_in_settings.readonly(),
            upper_solid);
        if (dense_area.empty())
            continue;

        // Avoid overlapping dense pieces when several region settings touch
        // the same source surface. The pre-diff intersection decides whether a
        // later dense piece should receive a larger print priority.
        StoredExPolygonCollection dense_touch_area =
            offset_collection(storage, dense_area.readonly(), double(SCALED_EPSILON));
        const bool touches_previous =
            !result.dense_union.empty() &&
            !clipper_intersection(
                 clipper(dense_touch_area.readonly()),
                 clipper(result.dense_union.readonly())).empty();
        if (touches_previous)
            ++priority;

        if (!result.dense_union.empty())
            dense_area = diff_collection(storage, dense_area.readonly(), result.dense_union.readonly());
        dense_area = union_collection(storage, dense_area.readonly());
        if (dense_area.empty())
            continue;

        result.dense_union = result.dense_union.empty() ?
            dense_area.readonly().clone(storage) :
            clipper_union2(clipper(result.dense_union.readonly()), clipper(dense_area.readonly())).to_expolygon_collection();

        result.dense_areas.push_back(DenseArea{std::move(dense_area), priority});
    }

    return result;
}

void append_dense_surfaces(orchestrator_handle *orchestrator,
                           StoredSurfaceCollection &output,
                           const Surface &source,
                           DenseArea &dense_area)
{
    if (dense_area.areas.empty())
        return;

    const uint32_t first_new_idx = output.size();
    output.append_move(std::move(dense_area.areas), k_dense_surface_type);
    for (uint32_t idx = first_new_idx; idx < output.size(); ++idx) {
        MutableSurface surface = output.mutable_at(idx);
        surface.copy_properties_from(source);
        SurfaceDenseInfillHint &hint = surface.get_or_add_property<SurfaceDenseInfillHint>(orchestrator);
        hint.max_solid_layers_on_top = 1;
        hint.priority = dense_area.priority;
    }
}

void append_sparse_remainder(StoredSurfaceCollection &output,
                             storage_handle *storage,
                             const Surface &source,
                             const ExPolygonCollection &dense_union)
{
    StoredExPolygonCollection source_area(storage, source.expolygon());
    StoredExPolygonCollection sparse =
        dense_union.empty() ?
        std::move(source_area) :
        diff_collection(storage, source_area.readonly(), dense_union);

    if (!sparse.empty())
        output.append_like_move(std::move(sparse), source);
}

void process_region_island_surfaces(const run_ctx_surface_generation &ctx,
                                    storage_handle *storage,
                                    orchestrator_handle *orchestrator,
                                    const Print &print,
                                    const LayerIsland &island,
                                    const LayerRegionIsland &region_island,
                                    const ExPolygonCollection &upper_solid)
{
    const SurfaceCollection input = region_island.fill_surfaces_collection();
    if (input.empty())
        return;

    const std::vector<LayerRegion> regions = region_island.regions();
    if (regions.empty())
        return;

    StoredSurfaceCollection output(storage);
    bool changed = false;
    const LayerRegion primary_region = regions.front();
    for (const Surface surface : input) {
        DenseAreaResult dense =
            build_dense_areas_for_surface(storage, print, island, primary_region, surface, upper_solid);
        if (dense.dense_areas.empty()) {
            output.append_like(surface.expolygon(), surface);
            continue;
        }

        changed = true;
        for (DenseArea &dense_area : dense.dense_areas)
            append_dense_surfaces(orchestrator, output, surface, dense_area);
        append_sparse_remainder(output, storage, surface, dense.dense_union.readonly());
    }

    if (changed && ctx.set_region_island_fill_surfaces != nullptr)
        ctx.set_region_island_fill_surfaces(
            const_cast<layer_region_island_handle *>(region_island.handle()),
            output.mutable_handle());
}

void process_surface_marker_layer(uint32_t layer_idx,
                                  storage_handle *scratch_storage,
                                  const run_ctx_surface_generation *ctx,
                                  orchestrator_handle *orchestrator)
{
    assert(ctx != nullptr);
    assert(orchestrator != nullptr);
    assert(ctx->print != nullptr);
    assert(ctx->object != nullptr);

    const Print print(ctx->print);
    const Object object(ctx->object);
    const Layer layer = object.layer(layer_idx);
    for (uint32_t island_idx = 0; island_idx < layer.island_count(); ++island_idx) {
        const LayerIsland island = layer.island(island_idx);
        const StoredExPolygonCollection upper_solid = collect_upper_solid_areas(scratch_storage, island);
        if (upper_solid.empty())
            continue;

        for (uint32_t region_island_idx = 0; region_island_idx < island.region_island_count(); ++region_island_idx)
            process_region_island_surfaces(
                *ctx,
                scratch_storage,
                orchestrator,
                print,
                island,
                island.region_island(region_island_idx),
                upper_solid.readonly());
    }
}

std::map<uint64_t, uint16_t> dense_priorities_for_island(const LayerIsland &island)
{
    std::map<uint64_t, uint16_t> out;
    if (SurfaceDenseInfillHint::property_type == PLUGIN_PROPERTY_TYPE_INVALID)
        return out;

    for (uint32_t region_island_idx = 0; region_island_idx < island.region_island_count(); ++region_island_idx) {
        const LayerRegionIsland region_island = island.region_island(region_island_idx);
        for (const Surface surface : region_island.fill_surfaces_collection()) {
            const SurfaceDenseInfillHint *hint = surface.property<SurfaceDenseInfillHint>();
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
        root.add_child(placeholder.mutable_view());
        root.remove_child(0);
    } else {
        root.clear_content();
        for (uint32_t child_idx : normal_children)
            root.add_child(original.child(child_idx));
    }

    return true;
}

void publish_dense_children_by_priority(const run_ctx_post_infill_generation &ctx,
                                        storage_handle *storage,
                                        const LayerIsland &island,
                                        DensePostInfillWork &work)
{
    if (work.dense_children_by_priority.empty() ||
        ctx.get_or_create_region_island == nullptr ||
        ctx.get_region_island_mutable_extrusion == nullptr)
        return;

    layer_region_island_handle *destination_region_island =
        ctx.get_or_create_region_island(
            island.handle(),
            work.destination_regions.empty() ? nullptr : work.destination_regions.data(),
            static_cast<uint32_t>(work.destination_regions.size()));
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
            group.add_child(child.readonly());
        group.disable_sort();
        group.disable_reverse();
        destination_root.add_child(group.mutable_view());
    }
}

void process_post_infill_layer(uint32_t layer_idx,
                               storage_handle *scratch_storage,
                               const run_ctx_post_infill_generation *ctx)
{
    assert(ctx != nullptr);
    assert(ctx->object != nullptr);
    assert(ctx->get_region_island_mutable_extrusion != nullptr);

    const Object object(ctx->object);
    const Layer layer = object.layer(layer_idx);
    for (uint32_t island_idx = 0; island_idx < layer.island_count(); ++island_idx) {
        const LayerIsland island = layer.island(island_idx);
        const std::map<uint64_t, uint16_t> priorities = dense_priorities_for_island(island);
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

plugin_property_type SurfaceDenseInfillHint::property_type = PLUGIN_PROPERTY_TYPE_INVALID;

DenseInfillSurfaceMarker &DenseInfillSurfaceMarker::instance(orchestrator_handle *orch)
{
    static DenseInfillSurfaceMarker plugin(orch);
    return plugin;
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

void DenseInfillSurfaceMarker::inilialize_impl(storage_handle *) const
{
    register_dense_hint_property(m_orchestrator);
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

    const Object object(ctx->object);
    parallel_for_storage_with_progress(
        0,
        object.layer_count(),
        run_ctx,
        &progress(),
        process_surface_marker_layer,
        ctx,
        m_orchestrator);
}

DenseInfillRecipeModifier &DenseInfillRecipeModifier::instance(orchestrator_handle *orch)
{
    static DenseInfillRecipeModifier plugin(orch);
    return plugin;
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

void DenseInfillRecipeModifier::inilialize_impl(storage_handle *) const
{
    register_dense_hint_property(m_orchestrator);
}

void DenseInfillRecipeModifier::run_impl(const plugin_run_context *run_ctx) const
{
    const run_ctx_infill_surface_recipe_modifier *ctx =
        plugin_ctx_as_infill_surface_recipe_modifier(run_ctx);
    if (ctx == nullptr || ctx->surface == nullptr || ctx->params == nullptr)
        return;

    const Surface surface(ctx->surface);
    const SurfaceDenseInfillHint *hint = surface.property<SurfaceDenseInfillHint>();
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

void DenseInfillPostInfillOrder::inilialize_impl(storage_handle *) const
{
    register_dense_hint_property(m_orchestrator);
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
        ctx);
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
