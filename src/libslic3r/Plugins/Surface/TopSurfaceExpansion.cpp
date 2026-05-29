///|/ Copyright (c) SuperSlicer 2026 Durand Remi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/

#include "TopSurfaceExpansion.hpp"

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <vector>

#include "libslic3r/Api/plugin/c/slic3r_orchestrator.h"
#include "libslic3r/Api/plugin/c/steps/slic3r_step_surface_generation.h"
#include "libslic3r/Api/plugin/cpp/ClipperViews.hpp"
#include "libslic3r/Api/plugin/cpp/DataTreeViews.hpp"
#include "libslic3r/Api/plugin/cpp/RegionSettingsViews.hpp"

namespace slic3r_api { namespace SurfaceGeneration { namespace TopSurfaceExpansionPlugin {
namespace {

const char *k_top_surface_expansion_id = "surface.top_surface_expansion";
const char *k_dependencies[] = { "surface.solid_shells", nullptr };

const char *k_external_infill_margin_key = "external_infill_margin";
const char *k_top_solid_layers_key = "top_solid_layers";
const char *k_top_solid_min_thickness_key = "top_solid_min_thickness";
const char *k_perimeters_key = "perimeters";

const raw_used_config_key k_used_config_keys[] = {
    { k_external_infill_margin_key, RAW_CO_FLOAT_OR_PERCENT, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE },
    { k_top_solid_layers_key, RAW_CO_INT, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE },
    { k_top_solid_min_thickness_key, RAW_CO_FLOAT, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE },
    { k_perimeters_key, RAW_CO_INT, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE }
};

constexpr raw_surface_type k_top_solid = RAW_SURFACE_TYPE_POS_TOP | RAW_SURFACE_TYPE_DENS_SOLID;
constexpr raw_surface_type k_internal_solid = RAW_SURFACE_TYPE_POS_INTERNAL | RAW_SURFACE_TYPE_DENS_SOLID;
constexpr raw_surface_type k_internal_sparse = RAW_SURFACE_TYPE_POS_INTERNAL | RAW_SURFACE_TYPE_DENS_SPARSE;
constexpr raw_surface_type k_internal_void = RAW_SURFACE_TYPE_POS_INTERNAL | RAW_SURFACE_TYPE_DENS_VOID;

bool is_top_surface(raw_surface_type type)
{
    return surface_type_is_top(type) && surface_type_is_solid(type);
}

bool is_rebuildable_internal_surface(raw_surface_type type)
{
    // The top-margin projection changes only ordinary internal sparse/void
    // infill. Bridge and already-solid areas are left intact: bridge expansion
    // and solid-shell detection have their own modules in this step.
    return surface_type_is_internal(type) &&
           !surface_type_is_bridge(type) &&
           !surface_type_is_solid(type);
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

StoredExPolygonCollection clipped_surface_area(storage_handle *storage,
                                               const Surface &surface,
                                               const RegionSettingsClip &settings_clip)
{
    StoredExPolygonCollection single = collection_from_expolygon(storage, surface.expolygon());
    return settings_clip.is_accept_all() ? single.readonly().clone(storage) :
                                           settings_clip.intersections(single.readonly());
}

StoredExPolygonCollection collect_surfaces(storage_handle *storage,
                                           const SurfaceCollection &surfaces,
                                           raw_surface_type type,
                                           const RegionSettingsClip &settings_clip)
{
    StoredExPolygonCollection out(storage);
    for (const Surface surface : surfaces) {
        if (surface.type() != type)
            continue;

        StoredExPolygonCollection clipped = clipped_surface_area(storage, surface, settings_clip);
        out.append_move_from(std::move(clipped));
    }
    return out;
}

StoredExPolygonCollection collect_top_surfaces(storage_handle *storage,
                                               const SurfaceCollection &surfaces,
                                               const RegionSettingsClip &settings_clip)
{
    StoredExPolygonCollection out(storage);
    for (const Surface surface : surfaces) {
        if (!is_top_surface(surface.type()))
            continue;

        StoredExPolygonCollection clipped = clipped_surface_area(storage, surface, settings_clip);
        out.append_move_from(std::move(clipped));
    }
    return out;
}

coord_t max_shell_width_reference(const LayerIsland &island)
{
    // external_infill_margin may be a percentage. The legacy meaning of that
    // percentage is "percentage of the perimeter shell width". Surface
    // generation only has region configs and flows, so this computes the same
    // reference conservatively from the island's regions.
    coord_t max_shell_width = 0;
    for (uint32_t region_idx = 0; region_idx < island.region_count(); ++region_idx) {
        const LayerRegion region = island.region(region_idx);
        const ConfigOption perimeters_option = region.print_region().config().get(k_perimeters_key);
        const int32_t perimeters = std::max<int32_t>(0, perimeters_option.get_int());
        if (perimeters <= 0)
            continue;

        const c_flow external_flow = region.flow(RAW_EXTRUSION_ROLE_EXTERNAL_PERIMETER);
        const c_flow perimeter_flow = region.flow(RAW_EXTRUSION_ROLE_INTERNAL_PERIMETER);
        const coord_t shell_width =
            coord_t(0.5 * double(external_flow.width) + double(external_flow.spacing)) +
            perimeter_flow.spacing * (perimeters - 1);
        max_shell_width = std::max(max_shell_width, shell_width);
    }
    return max_shell_width;
}

coord_t external_infill_margin(const RegionSettingsValue &settings, const coord_t shell_width)
{
    if (!settings.is_enabled(k_external_infill_margin_key))
        return 0;

    const double margin_mm =
        std::max(0.0, settings.get_effective_value(unscaled(shell_width), k_external_infill_margin_key));
    return scale_i(margin_mm);
}

bool candidate_contains_source(storage_handle *storage,
                               const ExPolygon &source,
                               const ExPolygon &candidate)
{
    // Clipper offset can occasionally create several components after being
    // clipped back to an island. The valid component is the one that still
    // contains the original top area; choosing another component would make the
    // top surface "jump" to a detached neighboring area.
    StoredExPolygonCollection source_collection = collection_from_expolygon(storage, source);
    StoredExPolygonCollection candidate_collection = collection_from_expolygon(storage, candidate);
    StoredExPolygonCollection missing =
        diff_collection(storage, source_collection.readonly(), candidate_collection.readonly());
    return missing.empty();
}

StoredExPolygonCollection expand_single_top_surface(storage_handle *storage,
                                                    const ExPolygon &source,
                                                    const coord_t margin,
                                                    const ExPolygonCollection &domain)
{
    assert(storage != nullptr);

    StoredExPolygonCollection source_collection = collection_from_expolygon(storage, source);
    if (margin <= 0)
        return source_collection.readonly().clone(storage);

    ClipperContext clip(storage);
    StoredExPolygonCollection expanded =
        clipper_offset(clip(source_collection.readonly()), double(margin)).to_expolygon_collection();
    expanded = intersection_collection(storage, expanded.readonly(), domain);

    StoredExPolygonCollection kept(storage);
    for (const ExPolygon candidate : expanded) {
        if (candidate_contains_source(storage, source, candidate))
            kept.push_back(candidate);
    }

    // A positive margin should normally preserve the source. If numerical
    // cleanup removes every candidate, keep the unexpanded source so the plugin
    // never deletes an existing top surface.
    if (kept.empty())
        kept.push_back(source);
    return kept;
}

StoredExPolygonCollection expand_top_surfaces(storage_handle *storage,
                                              const ExPolygonCollection &top_surfaces,
                                              const coord_t margin,
                                              const ExPolygonCollection &domain)
{
    StoredExPolygonCollection expanded(storage);
    for (const ExPolygon top_surface : top_surfaces) {
        StoredExPolygonCollection one = expand_single_top_surface(storage, top_surface, margin, domain);
        expanded.append_move_from(std::move(one));
    }
    return union_collection(storage, expanded.readonly());
}

StoredExPolygonCollection top_margin_ring(storage_handle *storage,
                                          const ExPolygonCollection &top_surfaces,
                                          const coord_t margin,
                                          const ExPolygonCollection &domain)
{
    if (top_surfaces.empty() || margin <= 0)
        return StoredExPolygonCollection(storage);

    StoredExPolygonCollection expanded = expand_top_surfaces(storage, top_surfaces, margin, domain);
    StoredExPolygonCollection original = union_collection(storage, top_surfaces);
    return diff_collection(storage, expanded.readonly(), original.readonly());
}

bool include_lower_top_shell_layer(const Layer &current_layer,
                                   const Layer &upper_layer,
                                   const uint32_t distance,
                                   const int32_t top_solid_layers,
                                   const coord_t min_thickness)
{
    // The top layer itself has distance 0. For a lower layer looking upward,
    // distance 1 is the first layer above it, so top_solid_layers=2 means this
    // current layer should receive material from that upper top surface.
    return (top_solid_layers > 0 && int32_t(distance) < top_solid_layers) ||
           (min_thickness > 0 && upper_layer.print_z() - current_layer.print_z() < min_thickness);
}

StoredExPolygonCollection top_margin_rings_from_layer(storage_handle *storage,
                                                      const Layer &upper_layer,
                                                      const coord_t margin)
{
    StoredExPolygonCollection rings(storage);
    if (margin <= 0)
        return rings;

    for (uint32_t island_idx = 0; island_idx < upper_layer.island_count(); ++island_idx) {
        const LayerIsland island = upper_layer.island(island_idx);
        RegionSettingsClip accept_all(storage);
        StoredExPolygonCollection top_surfaces(storage);
        for (uint32_t region_island_idx = 0; region_island_idx < island.region_island_count(); ++region_island_idx) {
            StoredExPolygonCollection region_top =
                collect_top_surfaces(storage,
                                     island.region_island(region_island_idx).fill_surfaces_collection(),
                                     accept_all);
            top_surfaces.append_move_from(std::move(region_top));
        }

        StoredExPolygonCollection island_rings =
            top_margin_ring(storage, top_surfaces.readonly(), margin, island.infill_areas());
        rings.append_move_from(std::move(island_rings));
    }
    return union_collection(storage, rings.readonly());
}

StoredExPolygonCollection projected_top_margin_rings(storage_handle *storage,
                                                     const Object &object,
                                                     const uint32_t current_layer_idx,
                                                     const RegionSettingsValue &settings,
                                                     const coord_t shell_width)
{
    const int32_t top_solid_layers = std::max<int32_t>(0, settings.get_int(k_top_solid_layers_key));
    const coord_t min_thickness =
        scale_to_layer_coord(std::max(0.0, settings.get_float(k_top_solid_min_thickness_key)));
    if (top_solid_layers == 0 && min_thickness == 0)
        return StoredExPolygonCollection(storage);

    const coord_t margin = external_infill_margin(settings, shell_width);
    if (margin <= 0)
        return StoredExPolygonCollection(storage);

    const Layer current_layer = object.layer(current_layer_idx);
    StoredExPolygonCollection rings(storage);
    for (uint32_t upper_idx = current_layer_idx + 1; upper_idx < object.layer_count(); ++upper_idx) {
        const uint32_t distance = upper_idx - current_layer_idx;
        const Layer upper_layer = object.layer(upper_idx);
        if (!include_lower_top_shell_layer(current_layer, upper_layer, distance, top_solid_layers, min_thickness))
            break;

        StoredExPolygonCollection upper_rings = top_margin_rings_from_layer(storage, upper_layer, margin);
        rings.append_move_from(std::move(upper_rings));
    }
    return union_collection(storage, rings.readonly());
}

void append_surfaces_not_rebuilt(StoredSurfaceCollection &out,
                                 storage_handle *storage,
                                 const SurfaceCollection &input_surfaces)
{
    // Lower-layer propagation rebuilds only ordinary internal sparse/void
    // surfaces. Everything else is copied before per-region settings clips are
    // processed, so the plugin can split only the surfaces it actually owns.
    for (const Surface surface : input_surfaces) {
        if (!is_rebuildable_internal_surface(surface.type())) {
            StoredExPolygonCollection single = collection_from_expolygon(storage, surface.expolygon());
            append_surface_group(out, single.readonly(), surface.type());
        }
    }
}

void append_projected_margin_result(StoredSurfaceCollection &out,
                                    storage_handle *storage,
                                    const ExPolygonCollection &source,
                                    const ExPolygonCollection &projected_rings,
                                    raw_surface_type residual_type)
{
    if (source.empty())
        return;

    StoredExPolygonCollection solid =
        intersection_collection(storage, source, projected_rings);
    StoredExPolygonCollection residual =
        diff_collection(storage, source, solid.readonly());

    append_surface_group(out, solid.readonly(), k_internal_solid);
    append_surface_group(out, residual.readonly(), residual_type);
}

void propagate_top_margin_to_region_island(const run_ctx_surface_generation &ctx,
                                           storage_handle *storage,
                                           const Object &object,
                                           const LayerIsland &island,
                                           const uint32_t layer_idx,
                                           const LayerRegionIsland &region_island)
{
    const SurfaceCollection input_surfaces = region_island.fill_surfaces_collection();
    if (input_surfaces.empty())
        return;

    RegionSettings settings(storage, island,
        {{ k_external_infill_margin_key, k_top_solid_layers_key, k_top_solid_min_thickness_key }});
    settings.segregate(island.slice());

    const coord_t shell_width = max_shell_width_reference(island);
    StoredSurfaceCollection output(storage);
    append_surfaces_not_rebuilt(output, storage, input_surfaces);

    const RegionSettings::AreaMap &areas = settings.get_areas(k_external_infill_margin_key);
    for (const std::pair<const RegionSettingsValue, RegionSettingsClip> &entry : areas) {
        StoredExPolygonCollection sparse =
            collect_surfaces(storage, input_surfaces, k_internal_sparse, entry.second);
        StoredExPolygonCollection empty =
            collect_surfaces(storage, input_surfaces, k_internal_void, entry.second);
        if (sparse.empty() && empty.empty())
            continue;

        StoredExPolygonCollection projected_rings =
            projected_top_margin_rings(storage, object, layer_idx, entry.first, shell_width);
        StoredExPolygonCollection projected_inside_island =
            intersection_collection(storage, projected_rings.readonly(), island.infill_areas());

        // Each source type is rebuilt independently so sparse and void areas
        // keep their meaning outside the new margin. The solid pieces use one
        // common type because both are now part of the top shell support.
        append_projected_margin_result(output,
                                       storage,
                                       sparse.readonly(),
                                       projected_inside_island.readonly(),
                                       k_internal_sparse);
        append_projected_margin_result(output,
                                       storage,
                                       empty.readonly(),
                                       projected_inside_island.readonly(),
                                       k_internal_void);
    }

    if (ctx.set_region_island_fill_surfaces != nullptr)
        ctx.set_region_island_fill_surfaces(
            const_cast<layer_region_island_handle *>(region_island.handle()),
            output.mutable_handle());
}

void append_same_layer_non_top_results(StoredSurfaceCollection &out,
                                       storage_handle *storage,
                                       const SurfaceCollection &input_surfaces,
                                       const ExPolygonCollection &expanded_top)
{
    // Once top surfaces are expanded, every other surface is reduced by the
    // same union. This keeps the LayerRegionIsland fill surfaces a clean
    // partition for infill while allowing top infill to overlap the former
    // internal/bottom classification.
    for (const Surface surface : input_surfaces) {
        if (is_top_surface(surface.type()))
            continue;

        StoredExPolygonCollection single = collection_from_expolygon(storage, surface.expolygon());
        StoredExPolygonCollection residual = diff_collection(storage, single.readonly(), expanded_top);
        append_surface_group(out, residual.readonly(), surface.type());
    }
}

void expand_top_surfaces_in_region_island(const run_ctx_surface_generation &ctx,
                                          storage_handle *storage,
                                          const LayerIsland &island,
                                          const LayerRegionIsland &region_island)
{
    const SurfaceCollection input_surfaces = region_island.fill_surfaces_collection();
    if (input_surfaces.empty())
        return;

    RegionSettings settings(storage, island, {{ k_external_infill_margin_key }});
    settings.segregate(island.slice());

    const coord_t shell_width = max_shell_width_reference(island);
    StoredExPolygonCollection expanded_top(storage);
    const RegionSettings::AreaMap &areas = settings.get_areas(k_external_infill_margin_key);
    for (const std::pair<const RegionSettingsValue, RegionSettingsClip> &entry : areas) {
        StoredExPolygonCollection source_top = collect_top_surfaces(storage, input_surfaces, entry.second);
        const coord_t margin = external_infill_margin(entry.first, shell_width);
        StoredExPolygonCollection expanded =
            expand_top_surfaces(storage, source_top.readonly(), margin, island.infill_areas());
        expanded_top.append_move_from(std::move(expanded));
    }
    expanded_top = union_collection(storage, expanded_top.readonly());
    if (expanded_top.empty())
        return;

    StoredSurfaceCollection output(storage);
    append_surface_group(output, expanded_top.readonly(), k_top_solid);
    append_same_layer_non_top_results(output, storage, input_surfaces, expanded_top.readonly());

    if (ctx.set_region_island_fill_surfaces != nullptr)
        ctx.set_region_island_fill_surfaces(
            const_cast<layer_region_island_handle *>(region_island.handle()),
            output.mutable_handle());
}

void process_lower_layer_projection(const run_ctx_surface_generation &ctx,
                                    storage_handle *storage,
                                    const Object &object,
                                    const uint32_t layer_idx)
{
    const Layer layer = object.layer(layer_idx);
    for (uint32_t island_idx = 0; island_idx < layer.island_count(); ++island_idx) {
        const LayerIsland island = layer.island(island_idx);
        for (uint32_t region_island_idx = 0; region_island_idx < island.region_island_count(); ++region_island_idx)
            propagate_top_margin_to_region_island(ctx,
                                                  storage,
                                                  object,
                                                  island,
                                                  layer_idx,
                                                  island.region_island(region_island_idx));
    }
}

void process_same_layer_top_expansion(const run_ctx_surface_generation &ctx,
                                      storage_handle *storage,
                                      const Object &object,
                                      const uint32_t layer_idx)
{
    const Layer layer = object.layer(layer_idx);
    for (uint32_t island_idx = 0; island_idx < layer.island_count(); ++island_idx) {
        const LayerIsland island = layer.island(island_idx);
        for (uint32_t region_island_idx = 0; region_island_idx < island.region_island_count(); ++region_island_idx)
            expand_top_surfaces_in_region_island(ctx,
                                                 storage,
                                                 island,
                                                 island.region_island(region_island_idx));
    }
}

} // namespace

TopSurfaceExpansion &TopSurfaceExpansion::instance(orchestrator_handle *orch)
{
    static TopSurfaceExpansion s_instance(orch);
    return s_instance;
}

const char *TopSurfaceExpansion::id_impl() const noexcept
{
    return k_top_surface_expansion_id;
}

const char *TopSurfaceExpansion::name_impl() const noexcept
{
    return "Top surface expansion";
}

const char *TopSurfaceExpansion::description_impl() const noexcept
{
    return "Expands top infill surfaces by external_infill_margin and carries that margin into top shell layers.";
}

slicing_step_t TopSurfaceExpansion::step_impl() const noexcept
{
    return STEP_SURFACE_GENERATION;
}

const char *const *TopSurfaceExpansion::dependencies_impl() const noexcept
{
    return k_dependencies;
}

int32_t TopSurfaceExpansion::priority_impl() const noexcept
{
    return 20;
}

int32_t TopSurfaceExpansion::used_config_keys(raw_used_config_key *keys) const noexcept
{
    if (keys != nullptr)
        for (uint32_t idx = 0; idx < sizeof(k_used_config_keys) / sizeof(k_used_config_keys[0]); ++idx)
            keys[idx] = k_used_config_keys[idx];
    return int32_t(sizeof(k_used_config_keys) / sizeof(k_used_config_keys[0]));
}

const char *TopSurfaceExpansion::progress_message_format_impl() const noexcept
{
    return "Expand top surfaces: %u / %u layer passes";
}

void TopSurfaceExpansion::setup_run_impl(const plugin_run_context *run_ctx) const
{
    const run_ctx_surface_generation *ctx = plugin_ctx_as_surface_generation(run_ctx);
    if (ctx != nullptr && ctx->object != nullptr) {
        const Object object(ctx->object);
        progress().add_max(object.layer_count() * 2);
    }
}

void TopSurfaceExpansion::run_impl(const plugin_run_context *run_ctx) const
{
    const run_ctx_surface_generation *ctx = plugin_ctx_as_surface_generation(run_ctx);
    if (ctx == nullptr || ctx->object == nullptr || run_ctx == nullptr || run_ctx->plugin_storage == nullptr)
        return;

    const Object object(ctx->object);

    // Pass 1 reads top surfaces from upper layers and modifies only lower
    // internal surfaces. Keeping top surfaces untouched during this pass avoids
    // using an already-expanded top as the source for another expansion.
    for (uint32_t layer_idx = 0; layer_idx < object.layer_count(); ++layer_idx) {
        throw_if_cancelled(run_ctx);
        process_lower_layer_projection(*ctx, run_ctx->plugin_storage, object, layer_idx);
        progress().increment();
    }

    // Pass 2 finally expands the top surfaces on their own layer and diffs all
    // sibling surfaces by the expanded top union so the fill-surface partition
    // remains non-overlapping.
    for (uint32_t layer_idx = 0; layer_idx < object.layer_count(); ++layer_idx) {
        throw_if_cancelled(run_ctx);
        process_same_layer_top_expansion(*ctx, run_ctx->plugin_storage, object, layer_idx);
        progress().increment();
    }
}

void register_top_surface_expansion_plugin(orchestrator_handle *orch)
{
    orchestrator_register_plugin(orch, TopSurfaceExpansion::instance(orch).c_instance());
}

}}} // namespace slic3r_api::SurfaceGeneration::TopSurfaceExpansionPlugin

#ifdef TOP_SURFACE_EXPANSION_PLUGIN_DLL
extern "C" void register_plugin(orchestrator_handle *orch)
{
    slic3r_api::SurfaceGeneration::TopSurfaceExpansionPlugin::register_top_surface_expansion_plugin(orch);
}
#endif // TOP_SURFACE_EXPANSION_PLUGIN_DLL
