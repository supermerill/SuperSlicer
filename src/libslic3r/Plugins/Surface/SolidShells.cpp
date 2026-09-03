///|/ Copyright (c) SuperSlicer 2026 Durand Remi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/

#include "SolidShells.hpp"

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "libslic3r/Api/plugin/c/slic3r_orchestrator.h"
#include "libslic3r/Api/plugin/c/steps/slic3r_step_surface_generation.h"
#include "libslic3r/Api/plugin/cpp/ClipperViews.hpp"
#include "libslic3r/Api/plugin/cpp/SurfaceViews.hpp"
#include "libslic3r/Api/plugin/cpp/ParallelFor.hpp"
#include "libslic3r/Api/plugin/cpp/RegionSettingsViews.hpp"

/*
Solid shell surfaces
====================

This STEP_SURFACE_GENERATION plugin classifies internal sparse or void areas
as solid where the configured top or bottom shell thickness requires material.
It consumes typed fill surfaces produced by the initial surface builder and
publishes a replacement surface partition. It does not generate infill or
change perimeter and island geometry.

The normal execution flow is:

    setup_run()
    `-- prepare progress for the object's layers

    run_impl()
    |-- validate that every non-empty fill collection is typed
    `-- process_layer() for every object layer
        `-- for every LayerRegionIsland:
            |-- segregate top/bottom shell settings by region area
            |-- keep bridges, existing solids, and non-internal surfaces intact
            |-- collect eligible internal sparse/void source areas
            |-- project exposed areas from upper layers for top shells
            |-- project exposed areas from lower layers for bottom shells
            |-- exclude portions already supported by the configured perimeter stack
            |-- union top and bottom candidates
            |-- emit internal solid and remaining internal sparse surfaces
            `-- replace the complete fill-surface collection

Layer-count and minimum-thickness settings are evaluated independently in both
directions. `solid_over_perimeters` prevents a shell request from creating
solid infill where enough perimeter coverage already provides the required
support. Special surfaces and regions with incompatible settings are preserved
or processed through their matching setting area; disabled settings therefore
leave the corresponding internal surface unchanged.
*/

namespace slic3r_api { namespace SurfaceGeneration { namespace SolidShellsPlugin {
namespace {

// This module reads the LayerRegionIsland fill surfaces produced earlier in
// STEP_SURFACE_GENERATION, keeps already-special surfaces unchanged, and only
// reclassifies internal sparse/void areas as internal solid.
const char *k_solid_shells_id = "surface.solid_shells";
const char *k_no_dependencies[] = { nullptr };

const char *k_top_solid_layers_key = "top_solid_layers";
const char *k_top_solid_min_thickness_key = "top_solid_min_thickness";
const char *k_bottom_solid_layers_key = "bottom_solid_layers";
const char *k_bottom_solid_min_thickness_key = "bottom_solid_min_thickness";
const char *k_solid_over_perimeters_key = "solid_over_perimeters";

const raw_used_config_key k_used_config_keys[] = {
    { k_top_solid_layers_key, RAW_CO_INT, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE },
    { k_top_solid_min_thickness_key, RAW_CO_FLOAT, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE },
    { k_bottom_solid_layers_key, RAW_CO_INT, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE },
    { k_bottom_solid_min_thickness_key, RAW_CO_FLOAT, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE },
    { k_solid_over_perimeters_key, RAW_CO_INT, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE }
};

constexpr raw_surface_type k_internal_solid = RAW_SURFACE_TYPE_POS_INTERNAL | RAW_SURFACE_TYPE_DENS_SOLID;
constexpr raw_surface_type k_internal_sparse = RAW_SURFACE_TYPE_POS_INTERNAL | RAW_SURFACE_TYPE_DENS_SPARSE;

bool processable_internal_surface(raw_surface_type type)
{
    // Bridges are refined by a dedicated bridge module. This plugin only
    // decides whether ordinary internal infill area should be sparse/void or
    // solid. Existing solid areas are left untouched to avoid re-splitting work
    // done by previous surface-generation plugins.
    return surface_type_is_internal(type) &&
           !surface_type_is_bridge(type) &&
           !surface_type_is_solid(type);
}

bool scan_surface_prerequisites(const SurfaceCollection &surfaces)
{
    if (surfaces.empty())
        return true;

    // Surface-generation plugins after the initial builder need typed,
    // non-empty areas. Missing position/density bits usually mean that an
    // earlier plugin skipped the initial classification step.
    for (const Surface surface : surfaces) {
        if (surface.expolygon().contour().empty())
            return true;
        if (!surface_type_has_any_flag(surface.type(),
                                       RAW_SURFACE_TYPE_POS_INTERNAL |
                                       RAW_SURFACE_TYPE_POS_TOP |
                                       RAW_SURFACE_TYPE_POS_BOTTOM))
            return true;
        if (!surface_type_has_any_flag(surface.type(), k_surface_type_density_flags))
            return true;
    }
    return false;
}

bool scan_object_surface_prerequisites(const Object &object)
{
    for (uint32_t layer_idx = 0; layer_idx < object.layer_count(); ++layer_idx) {
        const Layer layer = object.layer(layer_idx);
        for (uint32_t island_idx = 0; island_idx < layer.island_count(); ++island_idx) {
            const LayerIsland island = layer.island(island_idx);
            // If no fill area exists, no surface is expected for that island.
            if (island.infill_areas().empty())
                continue;
            for (uint32_t region_island_idx = 0; region_island_idx < island.region_island_count();
                 ++region_island_idx) {
                if (scan_surface_prerequisites(island.region_island(region_island_idx).fill_surfaces_collection()))
                    return true;
            }
        }
    }
    return false;
}

bool validate_surface_prerequisites(const plugin_run_context *run_ctx, const Object &object)
{
    const bool error_found = scan_object_surface_prerequisites(object);
    if (!error_found)
        return true;

    std::string message =
        "Solid shell surfaces requires typed fill surfaces from the initial surface builder before it can run.";
    message += " No LayerRegionIsland fill surfaces were found.";
    report_error(run_ctx, message.c_str());
    return false;
}

ClipperOperand linked_island_slices_shape(storage_handle *storage, const std::vector<LayerIsland> &linked_islands)
{
    ClipperContext clipper(storage);
    ClipperOperand slices = ClipperOperand::create_empty(storage);
    for (const LayerIsland &linked_island : linked_islands)
        slices += clipper(linked_island.slice());
    return slices;
}

ClipperOperand exposed_island_shape(storage_handle *storage,
                                    const LayerIsland &island,
                                    const bool top_side)
{
    // A top area is the part of this island not covered by islands above it.
    // A bottom area is the same test against islands below it. These exposed
    // areas are projected through neighboring layers to request solid shells.
    ClipperContext clipper(storage);
    const std::vector<LayerIsland> linked_islands = top_side ? island.upper_islands() : island.lower_islands();
    if (linked_islands.empty())
        return clipper(island.slice());

    ClipperOperand linked_slices = linked_island_slices_shape(storage, linked_islands);
    return clipper_diff(clipper(island.slice()), linked_slices);
}

ClipperOperand exposed_layer_shape(storage_handle *storage, const Layer &layer, const bool top_side)
{
    // Each island computes its own exposed area against the island overlap
    // graph. The final union gives a layer-wide projection target, which lets a
    // shell on one island solidify matching areas on a neighboring lower/upper
    // island when geometry overlaps after slicing.
    ClipperOperand exposed = ClipperOperand::create_empty(storage);
    for (uint32_t island_idx = 0; island_idx < layer.island_count(); ++island_idx) {
        ClipperOperand island_exposed = exposed_island_shape(storage, layer.island(island_idx), top_side);
        exposed += island_exposed;
    }
    return clipper_union(exposed);
}

ClipperOperand island_perimeter_shape(storage_handle *storage, const LayerIsland &island)
{
    // Perimeter-owned area is what remains between the full island slice and
    // the strict free infill area. If perimeters consumed the whole island,
    // infill_no_overlap_areas() is empty and the whole slice becomes perimeter.
    ClipperContext clipper(storage);
    return clipper_diff(clipper(island.slice()), clipper(island.infill_no_overlap_areas()));
}

ClipperOperand layer_perimeter_shape(storage_handle *storage, const Layer &layer)
{
    ClipperOperand perimeters = ClipperOperand::create_empty(storage);
    for (uint32_t island_idx = 0; island_idx < layer.island_count(); ++island_idx) {
        ClipperOperand island_perimeters = island_perimeter_shape(storage, layer.island(island_idx));
        perimeters += island_perimeters;
    }
    return clipper_union(perimeters);
}

bool include_top_layer(const Layer &current_layer,
                       const Layer &candidate_layer,
                       const uint32_t distance,
                       const int32_t top_solid_layers,
                       const coord_t min_thickness)
{
    // A candidate layer may be included by layer count or by physical shell
    // thickness. The first upper layer has distance 1, so top_solid_layers=2
    // means "the top layer plus the layer directly below it".
    return (top_solid_layers > 0 && int32_t(distance) < top_solid_layers) ||
           (min_thickness > 0 && candidate_layer.print_z() - current_layer.print_z() < min_thickness);
}

bool include_bottom_layer(const Layer &current_layer,
                          const Layer &candidate_layer,
                          const uint32_t distance,
                          const int32_t bottom_solid_layers,
                          const coord_t min_thickness)
{
    // Bottom thickness is measured between lower layer boundaries. This mirrors
    // the top shell test while keeping asymmetric layer heights correct.
    return (bottom_solid_layers > 0 && int32_t(distance) < bottom_solid_layers) ||
           (min_thickness > 0 && current_layer.bottom_z() - candidate_layer.bottom_z() < min_thickness);
}

ClipperOperand projected_top_shell_shape(storage_handle *storage,
                                         const Object &object,
                                         const uint32_t layer_idx,
                                         const RegionSettingsValue &settings)
{
    // Build the area that would become unsupported from above if this layer
    // stayed sparse. We collect exposed areas from the upper layers requested
    // by top_solid_layers/top_solid_min_thickness and project them onto the
    // current layer; the actual clipping to this layer's surfaces happens later.
    const int32_t top_solid_layers = std::max<int32_t>(0, settings.get_int(k_top_solid_layers_key));
    const coord_t min_thickness = scale_to_layer_coord(std::max(0.0, settings.get_float(k_top_solid_min_thickness_key)));
    if (top_solid_layers == 0 && min_thickness == 0)
        return ClipperOperand::create_empty(storage);

    const Layer current_layer = object.layer(layer_idx);
    ClipperOperand shell = ClipperOperand::create_empty(storage);
    for (uint32_t upper_idx = layer_idx + 1; upper_idx < object.layer_count(); ++upper_idx) {
        const uint32_t distance = upper_idx - layer_idx;
        const Layer upper_layer = object.layer(upper_idx);
        if (!include_top_layer(current_layer, upper_layer, distance, top_solid_layers, min_thickness))
            break;

        ClipperOperand exposed = exposed_layer_shape(storage, upper_layer, true);
        shell += exposed;
    }
    return clipper_union(shell);
}

ClipperOperand projected_bottom_shell_shape(storage_handle *storage,
                                            const Object &object,
                                            const uint32_t layer_idx,
                                            const RegionSettingsValue &settings)
{
    // Same idea as projected_top_shell(), but looking downward. Exposed bottom
    // areas from lower layers request solid material above them until the
    // configured bottom shell count or thickness is satisfied.
    const int32_t bottom_solid_layers = std::max<int32_t>(0, settings.get_int(k_bottom_solid_layers_key));
    const coord_t min_thickness =
        scale_to_layer_coord(std::max(0.0, settings.get_float(k_bottom_solid_min_thickness_key)));
    if (bottom_solid_layers == 0 && min_thickness == 0)
        return ClipperOperand::create_empty(storage);

    const Layer current_layer = object.layer(layer_idx);
    ClipperOperand shell = ClipperOperand::create_empty(storage);
    for (int32_t lower_idx = int32_t(layer_idx) - 1; lower_idx >= 0; --lower_idx) {
        const uint32_t distance = layer_idx - uint32_t(lower_idx);
        const Layer lower_layer = object.layer(uint32_t(lower_idx));
        if (!include_bottom_layer(current_layer, lower_layer, distance, bottom_solid_layers, min_thickness))
            break;

        ClipperOperand exposed = exposed_layer_shape(storage, lower_layer, false);
        shell += exposed;
    }
    return clipper_union(shell);
}

ClipperOperand perimeter_stack_coverage_shape(storage_handle *storage,
                                              const Object &object,
                                              const uint32_t layer_idx,
                                              const bool top_side,
                                              const int32_t solid_over_perimeters)
{
    // solid_over_perimeters prevents promoting a shell candidate when the whole
    // candidate is already backed by enough perimeter-owned material in the
    // neighboring layers. We intersect the perimeter areas of each required
    // adjacent layer; if any layer is missing or has no perimeter coverage, the
    // exemption cannot apply.
    if (solid_over_perimeters <= 0)
        return ClipperOperand::create_empty(storage);

    ClipperOperand coverage = ClipperOperand::create_empty(storage);
    bool has_coverage = false;
    for (int32_t step = 1; step <= solid_over_perimeters; ++step) {
        const int32_t adjacent_idx = top_side ? int32_t(layer_idx) + step : int32_t(layer_idx) - step;
        if (adjacent_idx < 0 || adjacent_idx >= int32_t(object.layer_count()))
            return ClipperOperand::create_empty(storage);

        ClipperOperand layer_perimeters = layer_perimeter_shape(storage, object.layer(uint32_t(adjacent_idx)));
        if (layer_perimeters.empty())
            return ClipperOperand::create_empty(storage);

        if (!has_coverage) {
            coverage = std::move(layer_perimeters);
            has_coverage = true;
            continue;
        }

        coverage = clipper_intersection(coverage, layer_perimeters);
        if (coverage.empty())
            return coverage;
    }
    return coverage;
}

bool fully_covered_by(storage_handle *storage, const ExPolygon &area, const ClipperOperand &coverage)
{
    // The exemption is all-or-nothing. Partial perimeter coverage is not enough
    // because the uncovered part still needs solid infill to carry the shell.
    if (coverage.empty())
        return false;

    ClipperContext clipper(storage);
    ClipperOperand uncovered = clipper_diff(clipper(area), coverage);
    return uncovered.empty();
}

StoredExPolygonCollection remove_fully_perimeter_covered_candidates(storage_handle *storage,
                                                                    const ExPolygonCollection &candidates,
                                                                    const ClipperOperand &perimeter_coverage)
{
    // Keep only candidates that still need solid infill. Fully perimeter-backed
    // candidates stay sparse so solid_over_perimeters behaves like the legacy
    // "do not waste solid infill under enough perimeters" rule.
    if (candidates.empty())
        return StoredExPolygonCollection(storage);
    if (perimeter_coverage.empty())
        return candidates.clone(storage);

    StoredExPolygonCollection out(storage);
    for (const ExPolygon candidate : candidates) {
        if (!fully_covered_by(storage, candidate, perimeter_coverage))
            out.push_back(candidate);
    }
    return out;
}

StoredExPolygonCollection solid_candidates_for_direction(storage_handle *storage,
                                                         const ClipperOperand &source,
                                                         const ClipperOperand &shell_zone,
                                                         const ClipperOperand &perimeter_coverage)
{
    // A top/bottom shell request only affects the part of the current sparse
    // source area that overlaps the projected exposed zone. The perimeter stack
    // filter is applied after clipping so the "fully covered" test is local to
    // each candidate polygon.
    if (source.empty() || shell_zone.empty())
        return StoredExPolygonCollection(storage);

    ClipperOperand candidate_shape = clipper_intersection(source, shell_zone);
    if (perimeter_coverage.empty())
        return candidate_shape.to_expolygon_collection();

    StoredExPolygonCollection candidates = candidate_shape.to_expolygon_collection();
    return remove_fully_perimeter_covered_candidates(storage, candidates.readonly(), perimeter_coverage);
}

StoredExPolygonCollection collect_processable_surfaces(storage_handle *storage,
                                                       const SurfaceCollection &surfaces,
                                                       const RegionSettingsClip &settings_clip)
{
    // RegionSettings may split an island into multiple clips, each with a
    // different combination of shell settings. Source surfaces are therefore
    // clipped to the active settings area before any top/bottom projection is
    // evaluated.
    StoredExPolygonCollection source(storage);
    for (const Surface surface : surfaces) {
        if (!processable_internal_surface(surface.type()))
            continue;

        settings_clip.append_intersections_to(source, surface.expolygon());
    }
    return source;
}

void append_unchanged_surfaces(StoredSurfaceCollection &out,
                               const SurfaceCollection &surfaces)
{
    // Preserve bridge, already-solid, and non-internal surfaces exactly as they
    // arrived. This keeps the plugin composable with other surface-generation
    // refinements that may run before or after it.
    for (const Surface surface : surfaces) {
        if (!processable_internal_surface(surface.type()))
            out.append(surface.expolygon(), surface.type());
    }
}

void append_solid_and_sparse_results(StoredSurfaceCollection &out,
                                     storage_handle *storage,
                                     StoredExPolygonCollection &&source,
                                     StoredExPolygonCollection &&top_solid,
                                     StoredExPolygonCollection &&bottom_solid)
{
    // Top and bottom requests produce the same final surface type, so they are
    // unioned before rebuilding the residual sparse area. This guarantees that
    // the output surfaces do not positively overlap.
    ClipperContext clipper(storage);
    ClipperOperand solid_shape = ClipperOperand::create_empty(storage);
    solid_shape += clipper(top_solid.readonly());
    solid_shape += clipper(bottom_solid.readonly());
    solid_shape = clipper_union(solid_shape);

    StoredExPolygonCollection solid = solid_shape.to_expolygon_collection();
    StoredExPolygonCollection sparse = clipper_diff(clipper(source.readonly()), solid_shape).to_expolygon_collection();
    out.append_move(std::move(solid), k_internal_solid);
    out.append_move(std::move(sparse), k_internal_sparse);
}

void rebuild_region_island_surfaces(const run_ctx_surface_generation &ctx,
                                    storage_handle *storage,
                                    const Object &object,
                                    const LayerIsland &island,
                                    const uint32_t layer_idx,
                                    const LayerRegionIsland &region_island)
{
    assert(storage != nullptr);

    // The step payload gives read-only access to the existing surfaces and a
    // callback for replacing the whole collection. That keeps ownership in the
    // host while still letting plugin code use temporary storage-owned geometry.
    const SurfaceCollection input_surfaces = region_island.fill_surfaces_collection();
    if (input_surfaces.empty())
        return;

    // All five settings are grouped together so every clipped area carries a
    // complete, self-consistent shell policy. Sparse and void input surfaces are
    // treated the same here; density-specific cleanup is left to later plugins.
    RegionSettings settings(storage, island,
        {{ k_top_solid_layers_key,
           k_top_solid_min_thickness_key,
           k_bottom_solid_layers_key,
           k_bottom_solid_min_thickness_key,
           k_solid_over_perimeters_key }});
    settings.segregate(island.slice());

    StoredSurfaceCollection output(storage);
    append_unchanged_surfaces(output, input_surfaces);

    const RegionSettings::AreaMap &areas = settings.get_areas(k_top_solid_layers_key);
    for (const auto &[setting_value, setting_clip] : areas) {
        // First collect only the source surfaces covered by this exact settings
        // combination. The same layer island can therefore have different solid
        // shell behavior in different regions without pre-splitting the island.
        StoredExPolygonCollection source = collect_processable_surfaces(storage, input_surfaces, setting_clip);
        if (source.empty())
            continue;

        const int32_t solid_over_perimeters =
            std::max<int32_t>(0, setting_value.get_int(k_solid_over_perimeters_key));

        ClipperContext clipper(storage);
        ClipperOperand source_shape = clipper(source.readonly());
        ClipperOperand top_shell = projected_top_shell_shape(storage, object, layer_idx, setting_value);
        ClipperOperand top_perimeter_coverage =
            perimeter_stack_coverage_shape(storage, object, layer_idx, true, solid_over_perimeters);
        StoredExPolygonCollection top_solid =
            solid_candidates_for_direction(storage, source_shape, top_shell, top_perimeter_coverage);

        // Bottom shell detection works on the part not already made solid by
        // the top pass. Both outputs use the same final Surface type, but this
        // avoids duplicated solid areas when top and bottom ranges overlap.
        ClipperOperand top_solid_shape = clipper(top_solid.readonly());
        ClipperOperand source_without_top = clipper_diff(source_shape, top_solid_shape);
        ClipperOperand bottom_shell = projected_bottom_shell_shape(storage, object, layer_idx, setting_value);
        ClipperOperand bottom_perimeter_coverage =
            perimeter_stack_coverage_shape(storage, object, layer_idx, false, solid_over_perimeters);
        StoredExPolygonCollection bottom_solid =
            solid_candidates_for_direction(storage,
                                           source_without_top,
                                           bottom_shell,
                                           bottom_perimeter_coverage);

        append_solid_and_sparse_results(output,
                                        storage,
                                        std::move(source),
                                        std::move(top_solid),
                                        std::move(bottom_solid));
    }

    if (ctx.set_region_island_fill_surfaces != nullptr) {
        // The collection was built in worker-local scratch storage. The callback
        // moves it into this LayerRegionIsland. The parallel loop assigns one
        // layer to each job, so two workers never publish to the same region island.
        ctx.set_region_island_fill_surfaces(
            const_cast<layer_region_island_handle *>(region_island.handle()),
            output.mutable_handle());
    }
}

void process_layer(const run_ctx_surface_generation &ctx,
                   storage_handle *storage,
                   const Object &object,
                   const uint32_t layer_idx)
{
    // Each LayerRegionIsland owns a fill-surface collection for one island and
    // one compatible region set. Rebuilding them independently avoids creating
    // surfaces that mix incompatible regional settings.
    const Layer layer = object.layer(layer_idx);
    for (uint32_t island_idx = 0; island_idx < layer.island_count(); ++island_idx) {
        const LayerIsland island = layer.island(island_idx);
        for (uint32_t region_island_idx = 0; region_island_idx < island.region_island_count(); ++region_island_idx)
            rebuild_region_island_surfaces(ctx,
                                           storage,
                                           object,
                                           island,
                                           layer_idx,
                                           island.region_island(region_island_idx));
    }
}

} // namespace

SolidShells &SolidShells::instance(orchestrator_handle *orch)
{
    static SolidShells s_instance(orch);
    return s_instance;
}

const char *SolidShells::id_impl() const noexcept
{
    return k_solid_shells_id;
}

const char *SolidShells::name_impl() const noexcept
{
    return "Solid shell surfaces";
}

const char *SolidShells::description_impl() const noexcept
{
    return "Turns internal surfaces into solid infill where top or bottom shell thickness requires it.";
}

slicing_step_t SolidShells::step_impl() const noexcept
{
    return STEP_SURFACE_GENERATION;
}

const char *const *SolidShells::dependencies_impl() const noexcept
{
    return k_no_dependencies;
}

int32_t SolidShells::priority_impl() const noexcept
{
    return 10;
}

int32_t SolidShells::used_config_keys(raw_used_config_key *keys) const noexcept
{
    if (keys != nullptr)
        for (uint32_t idx = 0; idx < sizeof(k_used_config_keys) / sizeof(k_used_config_keys[0]); ++idx)
            keys[idx] = k_used_config_keys[idx];
    return int32_t(sizeof(k_used_config_keys) / sizeof(k_used_config_keys[0]));
}

const char *SolidShells::progress_message_format_impl() const noexcept
{
    return "Build solid shell surfaces: %u / %u layers";
}

void SolidShells::setup_run_impl(const plugin_run_context *run_ctx) const
{
    if (run_ctx == nullptr)
        return;

    const run_ctx_surface_generation *ctx = plugin_ctx_as_surface_generation(run_ctx);
    if (ctx != nullptr && ctx->object != nullptr) {
        const Object object(ctx->object);
        progress().add_max(object.layer_count());
    }
}

void SolidShells::run_impl(const plugin_run_context *run_ctx) const
{
    if (run_ctx == nullptr)
        return;

    const run_ctx_surface_generation *ctx = plugin_ctx_as_surface_generation(run_ctx);
    if (ctx == nullptr || ctx->object == nullptr)
        return;

    const Object object(ctx->object);
    if (!validate_surface_prerequisites(run_ctx, object))
        return;

    // The helper handles cancellation, per-worker scratch storage, and progress
    // accounting. Each worker owns a layer, so rebuilt surfaces are published
    // into disjoint LayerRegionIsland objects.
    parallel_for_storage_with_progress(
        0,
        object.layer_count(),
        run_ctx,
        &progress(),
        [ctx, &object](const uint32_t layer_idx, storage_handle *scratch_storage) {
            assert(scratch_storage != nullptr);
            assert(ctx != nullptr);
            process_layer(*ctx, scratch_storage, object, layer_idx);
        });
}

void register_solid_shells_plugin(orchestrator_handle *orch)
{
    orchestrator_register_plugin(orch, SolidShells::instance(orch).c_instance());
}

}}} // namespace slic3r_api::SurfaceGeneration::SolidShellsPlugin

#ifdef SOLID_SHELLS_PLUGIN_DLL
extern "C" void register_plugin(orchestrator_handle *orch)
{
    slic3r_api::SurfaceGeneration::SolidShellsPlugin::register_solid_shells_plugin(orch);
}
#endif // SOLID_SHELLS_PLUGIN_DLL
