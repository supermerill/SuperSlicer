///|/ Copyright (c) SuperSlicer 2026 Durand Remi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/

#include "CleanInfillSurfaces.hpp"

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cmath>
#include <map>
#include <vector>

#include "libslic3r/Api/plugin/c/slic3r_orchestrator.h"
#include "libslic3r/Api/plugin/c/steps/slic3r_step_surface_generation.h"
#include "libslic3r/Api/plugin/cpp/ClipperViews.hpp"
#include "libslic3r/Api/plugin/cpp/DataTreeViews.hpp"
#include "libslic3r/Api/plugin/cpp/RegionSettingsViews.hpp"

namespace slic3r_api { namespace SurfaceGeneration { namespace CleanInfillSurfacesPlugin {
namespace {

const char *k_clean_infill_surfaces_id = "surface.clean_infill_surfaces";
const char *k_no_dependencies[] = { nullptr };
const char *k_solid_infill_below_layer_area_key = "solid_infill_below_layer_area";
const char *k_solid_infill_below_area_key = "solid_infill_below_area";
const char *k_solid_infill_below_width_key = "solid_infill_below_width";

const raw_used_config_key k_used_config_keys[] = {
    { k_solid_infill_below_layer_area_key, RAW_CO_FLOAT, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE },
    { k_solid_infill_below_area_key, RAW_CO_FLOAT, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE },
    { k_solid_infill_below_width_key, RAW_CO_FLOAT_OR_PERCENT, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE }
};

// Geometry at this size is numerical dust for infill generation. Keeping it as
// a separate Surface would only create fragile tiny infill jobs later.
constexpr double k_micro_surface_area = 10.0 * double(SCALED_EPSILON) * double(SCALED_EPSILON);

struct SurfaceAreaGroup
{
    raw_surface_type type;
    StoredExPolygonCollection areas;
};

bool is_sparse_surface(raw_surface_type type)
{
    return surface_type_is_sparse(type);
}

raw_surface_type solid_version(raw_surface_type type)
{
    // Keep the position and modifiers intact. Only the density changes from
    // sparse to solid, so a sparse top/bridge/internal surface keeps its role.
    return surface_type_replace_flags(type, k_surface_type_density_flags, RAW_SURFACE_TYPE_DENS_SOLID);
}

double area_sum(const ExPolygonCollection &areas)
{
    double out = 0.;
    for (const ExPolygon area : areas)
        out += std::abs(area.area());
    return out;
}

double scaled_area_threshold(const RegionSettingsValue &settings, const char *key)
{
    // UI area options are expressed in mm^2. ExPolygon::area() is in scaled^2,
    // so apply the length scale twice before comparing against geometry.
    return scale_d(scale_d(std::max(0.0, settings.get_float(key))));
}

StoredExPolygonCollection collection_from_expolygon(storage_handle *storage, const ExPolygon &expolygon)
{
    StoredExPolygonCollection out(storage);
    out.push_back(expolygon);
    return out;
}

StoredExPolygonCollection union_collection(storage_handle *storage, const ExPolygonCollection &areas)
{
    if (areas.empty())
        return StoredExPolygonCollection(storage);

    ClipperContext clip(storage);
    return clipper_union(clip(areas)).to_expolygon_collection();
}

StoredExPolygonCollection diff_collection(storage_handle *storage,
                                          const ExPolygonCollection &subject,
                                          const ExPolygonCollection &clip_areas)
{
    if (subject.empty())
        return StoredExPolygonCollection(storage);
    if (clip_areas.empty())
        return subject.clone(storage);

    ClipperContext clip(storage);
    return clipper_diff(clip(subject), clip(clip_areas)).to_expolygon_collection();
}

StoredExPolygonCollection intersection_collection(storage_handle *storage,
                                                  const ExPolygonCollection &subject,
                                                  const ExPolygonCollection &clip_areas)
{
    if (subject.empty() || clip_areas.empty())
        return StoredExPolygonCollection(storage);

    ClipperContext clip(storage);
    return clipper_intersection(clip(subject), clip(clip_areas)).to_expolygon_collection();
}

void append_surface_group(StoredSurfaceCollection &surfaces,
                          const ExPolygonCollection &areas,
                          raw_surface_type type)
{
    if (!areas.empty())
        surfaces.append(areas, type);
}

void append_single_surface(StoredSurfaceCollection &surfaces,
                           storage_handle *storage,
                           const Surface &surface)
{
    StoredExPolygonCollection single = collection_from_expolygon(storage, surface.expolygon());
    append_surface_group(surfaces, single.readonly(), surface.type());
}

double layer_infill_area(const Layer &layer)
{
    double out = 0.;
    for (uint32_t island_idx = 0; island_idx < layer.island_count(); ++island_idx)
        out += area_sum(layer.island(island_idx).infill_areas());
    return out;
}

StoredExPolygonCollection layer_area_promotion_zone(storage_handle *storage,
                                                    const Layer &layer,
                                                    const LayerIsland &island,
                                                    const RegionSettings &settings)
{
    // solid_infill_below_layer_area is a layer-wide decision: if the complete
    // fillable layer is tiny, all sparse surfaces using the matching setting
    // value become solid. RegionSettings still clips the resulting zone so
    // regions with a different threshold are not accidentally promoted.
    const double total_layer_area = layer_infill_area(layer);
    StoredExPolygonCollection zone(storage);
    const RegionSettings::AreaMap &areas = settings.get_areas(k_solid_infill_below_layer_area_key);
    for (const auto &[setting_value, setting_clip] : areas) {
        const double threshold = scaled_area_threshold(setting_value, k_solid_infill_below_layer_area_key);
        if (threshold <= 0. || total_layer_area > threshold)
            continue;

        StoredExPolygonCollection clipped = setting_clip.intersections(island.infill_areas());
        zone.append_move_from(std::move(clipped));
    }
    return union_collection(storage, zone.readonly());
}

StoredExPolygonCollection small_infill_area_promotion_zone(storage_handle *storage,
                                                           const LayerIsland &island,
                                                           const RegionSettings &settings)
{
    // solid_infill_below_area is checked against each individual infill area.
    // The area itself is the promotion zone, later clipped by the active
    // RegionSettings entry so modifiers can use different thresholds.
    StoredExPolygonCollection zone(storage);
    const RegionSettings::AreaMap &areas = settings.get_areas(k_solid_infill_below_area_key);
    for (const auto &[setting_value, setting_clip] : areas) {
        const double threshold = scaled_area_threshold(setting_value, k_solid_infill_below_area_key);
        if (threshold <= 0.)
            continue;

        for (const ExPolygon infill_area : island.infill_areas()) {
            if (std::abs(infill_area.area()) > threshold)
                continue;

            setting_clip.append_intersections_to(zone, infill_area);
        }
    }
    return union_collection(storage, zone.readonly());
}

StoredSurfaceCollection promote_sparse_inside_zone(storage_handle *storage,
                                                   const SurfaceCollection &input_surfaces,
                                                   const ExPolygonCollection &promotion_zone)
{
    // Convert only the sparse parts inside promotion_zone. Everything outside
    // the zone and every already-special surface keeps its current type.
    StoredSurfaceCollection output(storage);
    if (promotion_zone.empty()) {
        for (const Surface surface : input_surfaces)
            append_single_surface(output, storage, surface);
        return output;
    }

    for (const Surface surface : input_surfaces) {
        StoredExPolygonCollection source = collection_from_expolygon(storage, surface.expolygon());
        if (!is_sparse_surface(surface.type())) {
            append_surface_group(output, source.readonly(), surface.type());
            continue;
        }

        StoredExPolygonCollection solid = intersection_collection(storage, source.readonly(), promotion_zone);
        StoredExPolygonCollection residual = diff_collection(storage, source.readonly(), solid.readonly());
        append_surface_group(output, solid.readonly(), solid_version(surface.type()));
        append_surface_group(output, residual.readonly(), surface.type());
    }
    return output;
}

coord_t max_infill_spacing_reference(const LayerIsland &island)
{
    // Percent solid_infill_below_width values need a local width reference.
    // Sparse infill is the target of this cleanup, so use the largest internal
    // infill spacing from the island regions as the conservative reference.
    coord_t max_spacing = 0;
    for (uint32_t region_idx = 0; region_idx < island.region_count(); ++region_idx) {
        const c_flow flow = island.region(region_idx).flow(RAW_EXTRUSION_ROLE_INTERNAL_INFILL);
        max_spacing = std::max(max_spacing, flow.spacing);
    }
    return max_spacing;
}

coord_t solid_below_half_width(const RegionSettingsValue &settings, const coord_t reference_width)
{
    // A disabled nullable width option means "do not run the thin-width
    // cleanup for this setting area". Percent values are resolved against the
    // local sparse-infill spacing reference.
    if (!settings.is_enabled(k_solid_infill_below_width_key))
        return 0;

    const double width_mm =
        std::max(0.0, settings.get_effective_value(unscaled(reference_width), k_solid_infill_below_width_key));
    return scale_i(width_mm) / 2;
}

void append_thin_width_result(StoredSurfaceCollection &output,
                              storage_handle *storage,
                              const ExPolygonCollection &source,
                              const raw_surface_type source_type,
                              const coord_t half_width)
{
    if (source.empty())
        return;

    if (half_width <= 0) {
        append_surface_group(output, source, source_type);
        return;
    }

    // A negative/positive offset pair removes sparse parts that cannot contain
    // the requested width. The removed pieces are not discarded: they become
    // solid so narrow infill islands still receive material.
    ClipperContext clip(storage);
    StoredExPolygonCollection wide_sparse =
        clipper_offset2(clip(source), -double(half_width), double(half_width)).to_expolygon_collection();
    wide_sparse = intersection_collection(storage, wide_sparse.readonly(), source);

    StoredExPolygonCollection thin_solid = diff_collection(storage, source, wide_sparse.readonly());
    append_surface_group(output, thin_solid.readonly(), solid_version(source_type));
    append_surface_group(output, wide_sparse.readonly(), source_type);
}

StoredSurfaceCollection collapse_sparse_width(storage_handle *storage,
                                              const LayerIsland &island,
                                              const SurfaceCollection &input_surfaces,
                                              const RegionSettings &settings)
{
    // This pass is intentionally limited to stDensSparse surfaces. Top, bottom,
    // bridge, and already-solid surfaces may also be narrow, but their density
    // class has already been chosen by earlier surface-generation modules.
    StoredSurfaceCollection output(storage);
    const coord_t reference_width = max_infill_spacing_reference(island);
    const RegionSettings::AreaMap &areas = settings.get_areas(k_solid_infill_below_width_key);

    for (const Surface surface : input_surfaces) {
        if (!is_sparse_surface(surface.type())) {
            append_single_surface(output, storage, surface);
            continue;
        }

        if (areas.empty()) {
            append_single_surface(output, storage, surface);
            continue;
        }

        // RegionSettings normally covers the whole island for a known option,
        // but the cleanup must stay conservative if a future caller provides
        // only partial clips. Track what was processed and keep any residual
        // sparse area unchanged.
        StoredExPolygonCollection processed(storage);
        for (const auto &[setting_value, setting_clip] : areas) {
            StoredExPolygonCollection source = setting_clip.intersections(surface.expolygon());
            const coord_t half_width = solid_below_half_width(setting_value, reference_width);
            append_thin_width_result(output, storage, source.readonly(), surface.type(), half_width);
            processed.append_move_from(std::move(source));
        }

        StoredExPolygonCollection processed_union = union_collection(storage, processed.readonly());
        StoredExPolygonCollection surface_area = collection_from_expolygon(storage, surface.expolygon());
        StoredExPolygonCollection residual =
            diff_collection(storage, surface_area.readonly(), processed_union.readonly());
        append_surface_group(output, residual.readonly(), surface.type());
    }
    return output;
}

bool intersects_after_epsilon_offset(storage_handle *storage,
                                     const ExPolygon &lhs,
                                     const ExPolygon &rhs)
{
    // "Adjacent" is a geometric predicate, not just shared vertices. Expanding
    // by one scaled epsilon turns a touching border into a measurable overlap
    // while still rejecting unrelated micro islands.
    StoredExPolygonCollection lhs_single = collection_from_expolygon(storage, lhs);
    StoredExPolygonCollection rhs_single = collection_from_expolygon(storage, rhs);
    ClipperContext clip(storage);
    StoredExPolygonCollection expanded =
        clipper_offset(clip(lhs_single.readonly()), double(SCALED_EPSILON)).to_expolygon_collection();
    StoredExPolygonCollection overlap = intersection_collection(storage, expanded.readonly(), rhs_single.readonly());
    return !overlap.empty();
}

bool has_same_type_neighbor(storage_handle *storage,
                            const SurfaceCollection &surfaces,
                            const uint32_t surface_idx)
{
    const Surface candidate = surfaces[surface_idx];
    for (uint32_t other_idx = 0; other_idx < surfaces.size(); ++other_idx) {
        if (other_idx == surface_idx)
            continue;

        const Surface other = surfaces[other_idx];
        if (other.type() != candidate.type() || std::abs(other.expolygon().area()) <= k_micro_surface_area)
            continue;
        if (intersects_after_epsilon_offset(storage, candidate.expolygon(), other.expolygon()))
            return true;
    }
    return false;
}

StoredSurfaceCollection drop_isolated_micro_surfaces(storage_handle *storage,
                                                     const SurfaceCollection &input_surfaces)
{
    // Tiny fragments are usually numerical dust left by clipping. If they touch
    // a larger surface of the exact same type, keep them and let the final
    // union merge them. If they are isolated, dropping them is safer than
    // creating a separate infill island too small to print meaningfully.
    StoredSurfaceCollection output(storage);
    for (uint32_t surface_idx = 0; surface_idx < input_surfaces.size(); ++surface_idx) {
        const Surface surface = input_surfaces[surface_idx];
        const bool is_micro = std::abs(surface.expolygon().area()) <= k_micro_surface_area;
        if (is_micro && !has_same_type_neighbor(storage, input_surfaces, surface_idx))
            continue;

        append_single_surface(output, storage, surface);
    }
    return output;
}

StoredExPolygonCollection &areas_for_type(std::vector<SurfaceAreaGroup> &groups,
                                          storage_handle *storage,
                                          const raw_surface_type type)
{
    for (SurfaceAreaGroup &group : groups)
        if (group.type == type)
            return group.areas;

    groups.push_back(SurfaceAreaGroup{type, StoredExPolygonCollection(storage)});
    return groups.back().areas;
}

StoredSurfaceCollection normalize_same_type_surfaces(storage_handle *storage,
                                                     const SurfaceCollection &input_surfaces)
{
    // The cleanup phases above may split one surface into several pieces and
    // later create same-type neighbors again. Grouping by raw SurfaceType,
    // unioning, and rebuilding one Surface per ExPolygon restores a compact
    // partition before infill generation consumes the data.
    std::vector<SurfaceAreaGroup> groups;
    for (const Surface surface : input_surfaces)
        areas_for_type(groups, storage, surface.type()).push_back(surface.expolygon());

    StoredSurfaceCollection output(storage);
    for (SurfaceAreaGroup &group : groups) {
        StoredExPolygonCollection merged = union_collection(storage, group.areas.readonly());
        append_surface_group(output, merged.readonly(), group.type);
    }
    return output;
}

void set_region_island_surfaces(const run_ctx_surface_generation &ctx,
                                const LayerRegionIsland &region_island,
                                StoredSurfaceCollection &surfaces)
{
    // The surface-generation API moves the complete collection at once. This
    // avoids partial host/plugin ownership and makes each cleanup pass publish
    // a self-contained replacement partition.
    if (ctx.set_region_island_fill_surfaces != nullptr)
        ctx.set_region_island_fill_surfaces(
            const_cast<layer_region_island_handle *>(region_island.handle()),
            surfaces.mutable_handle());
}

void clean_region_island_surfaces(const run_ctx_surface_generation &ctx,
                                  storage_handle *storage,
                                  const Layer &layer,
                                  const LayerIsland &island,
                                  const LayerRegionIsland &region_island)
{
    const SurfaceCollection input = region_island.fill_surfaces_collection();
    if (input.empty())
        return;

    RegionSettings settings(storage, island,
        {{ k_solid_infill_below_layer_area_key },
         { k_solid_infill_below_area_key },
         { k_solid_infill_below_width_key }});
    settings.segregate(island.slice());

    // Phase 1: layer-wide tiny-fill rule. If the whole layer is below the
    // configured area threshold, sparse surfaces in matching regions become
    // solid before the more local rules are evaluated.
    StoredExPolygonCollection layer_zone =
        layer_area_promotion_zone(storage, layer, island, settings);
    StoredSurfaceCollection after_layer_area =
        promote_sparse_inside_zone(storage, input, layer_zone.readonly());

    // Phase 2: per-infill-area tiny-fill rule. Each ExPolygon in
    // island.infill_areas() is tested independently, which preserves large
    // sparse areas on the same layer while solidifying small isolated ones.
    StoredExPolygonCollection area_zone =
        small_infill_area_promotion_zone(storage, island, settings);
    StoredSurfaceCollection after_small_area =
        promote_sparse_inside_zone(storage, after_layer_area.readonly(), area_zone.readonly());

    // Phase 3: thin sparse cleanup. Sparse areas that cannot contain the
    // requested width are removed from sparse and reintroduced as solid.
    StoredSurfaceCollection after_width =
        collapse_sparse_width(storage, island, after_small_area.readonly(), settings);

    // Phase 4: discard isolated numerical dust. Tiny fragments touching a
    // larger same-type neighbor are kept because the next phase can merge them.
    StoredSurfaceCollection after_micro_cleanup =
        drop_isolated_micro_surfaces(storage, after_width.readonly());

    // Phase 5: normalize same-type surfaces. Later infill code should see a
    // compact partition, not a pile of fragments created by the cleanup steps.
    StoredSurfaceCollection normalized =
        normalize_same_type_surfaces(storage, after_micro_cleanup.readonly());
    set_region_island_surfaces(ctx, region_island, normalized);
}

void process_layer(const run_ctx_surface_generation &ctx,
                   storage_handle *storage,
                   const Object &object,
                   const uint32_t layer_idx)
{
    // Surface data lives on LayerRegionIsland. Iterate through every existing
    // region island and replace only its fill-surface collection; the island
    // geometry and region assignment remain owned by earlier steps.
    const Layer layer = object.layer(layer_idx);
    for (uint32_t island_idx = 0; island_idx < layer.island_count(); ++island_idx) {
        const LayerIsland island = layer.island(island_idx);
        for (uint32_t region_island_idx = 0; region_island_idx < island.region_island_count(); ++region_island_idx)
            clean_region_island_surfaces(ctx,
                                         storage,
                                         layer,
                                         island,
                                         island.region_island(region_island_idx));
    }
}

} // namespace

CleanInfillSurfaces &CleanInfillSurfaces::instance(orchestrator_handle *orch)
{
    static CleanInfillSurfaces s_instance(orch);
    return s_instance;
}

const char *CleanInfillSurfaces::id_impl() const noexcept
{
    return k_clean_infill_surfaces_id;
}

const char *CleanInfillSurfaces::name_impl() const noexcept
{
    return "Clean infill surfaces";
}

const char *CleanInfillSurfaces::description_impl() const noexcept
{
    return "Promotes tiny or too-narrow sparse infill surfaces to solid and merges same-type surface fragments.";
}

slicing_step_t CleanInfillSurfaces::step_impl() const noexcept
{
    return STEP_SURFACE_GENERATION;
}

const char *const *CleanInfillSurfaces::dependencies_impl() const noexcept
{
    return k_no_dependencies;
}

int32_t CleanInfillSurfaces::priority_impl() const noexcept
{
    return 100;
}

int32_t CleanInfillSurfaces::used_config_keys(raw_used_config_key *keys) const noexcept
{
    if (keys != nullptr)
        for (uint32_t idx = 0; idx < sizeof(k_used_config_keys) / sizeof(k_used_config_keys[0]); ++idx)
            keys[idx] = k_used_config_keys[idx];
    return int32_t(sizeof(k_used_config_keys) / sizeof(k_used_config_keys[0]));
}

const char *CleanInfillSurfaces::progress_message_format_impl() const noexcept
{
    return "Clean infill surfaces: %u / %u layers";
}

void CleanInfillSurfaces::setup_run_impl(const plugin_run_context *run_ctx) const
{
    const run_ctx_surface_generation *ctx = plugin_ctx_as_surface_generation(run_ctx);
    if (ctx != nullptr && ctx->object != nullptr) {
        const Object object(ctx->object);
        progress().add_max(object.layer_count());
    }
}

void CleanInfillSurfaces::run_impl(const plugin_run_context *run_ctx) const
{
    const run_ctx_surface_generation *ctx = plugin_ctx_as_surface_generation(run_ctx);
    if (ctx == nullptr || ctx->object == nullptr || run_ctx == nullptr || run_ctx->plugin_storage == nullptr)
        return;

    const Object object(ctx->object);
    for (uint32_t layer_idx = 0; layer_idx < object.layer_count(); ++layer_idx) {
        throw_if_cancelled(run_ctx);
        process_layer(*ctx, run_ctx->plugin_storage, object, layer_idx);
        progress().increment();
    }
}

void register_clean_infill_surfaces_plugin(orchestrator_handle *orch)
{
    orchestrator_register_plugin(orch, CleanInfillSurfaces::instance(orch).c_instance());
}

}}} // namespace slic3r_api::SurfaceGeneration::CleanInfillSurfacesPlugin

#ifdef CLEAN_INFILL_SURFACES_PLUGIN_DLL
extern "C" void register_plugin(orchestrator_handle *orch)
{
    slic3r_api::SurfaceGeneration::CleanInfillSurfacesPlugin::register_clean_infill_surfaces_plugin(orch);
}
#endif // CLEAN_INFILL_SURFACES_PLUGIN_DLL
