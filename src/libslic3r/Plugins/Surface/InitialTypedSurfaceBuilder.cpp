///|/ Copyright (c) SuperSlicer 2026 Durand Remi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/

#include "InitialTypedSurfaceBuilder.hpp"

#include <cassert>
#include <cstdint>
#include <map>
#include <vector>

#include "libslic3r/Api/plugin/c/slic3r_orchestrator.h"
#include "libslic3r/Api/plugin/c/steps/slic3r_step_surface_generation.h"
#include "libslic3r/Api/plugin/cpp/ClipperViews.hpp"
#include "libslic3r/Api/plugin/cpp/DataTreeViews.hpp"

namespace slic3r_api { namespace SurfaceGeneration { namespace InitialTypedSurfaceBuilderPlugin {
namespace {

const char *k_initial_typed_surface_builder_id = "surface.initial_typed_surface_builder";
const char *k_surface_generation_group = "step_surface_generation_plugin";
const char *k_no_dependencies[] = { nullptr };
constexpr raw_surface_type k_bottom_surface = RAW_SURFACE_TYPE_POS_BOTTOM | RAW_SURFACE_TYPE_DENS_SOLID;
constexpr raw_surface_type k_internal_surface = RAW_SURFACE_TYPE_POS_INTERNAL | RAW_SURFACE_TYPE_DENS_SPARSE;
constexpr raw_surface_type k_top_surface = RAW_SURFACE_TYPE_POS_TOP | RAW_SURFACE_TYPE_DENS_SOLID;

raw_surface_type bottom_surface_type(const bool is_first_layer)
{
    // A bottom surface on the first object layer is supported by the build
    // plate or raft. Bottom surfaces higher in the object are unsupported from
    // below and must carry the bridge modifier so bridge-specific surface
    // plugins and infill code can detect them without recomputing exposure.
    return is_first_layer ? k_bottom_surface : raw_surface_type(k_bottom_surface | RAW_SURFACE_TYPE_MOD_BRIDGE);
}

int32_t region_infill_extruder_id(const LayerRegion &region)
{
    // Config extruders are user-facing 1-based values. LayerRegionIsland stores
    // the resolved extruder as 0-based because the rest of the slicing pipeline
    // indexes extruders that way.
    const ConfigOption option = region.print_region().config().get("infill_extruder");
    const int32_t extruder_id = option.get_int() - 1;
    assert(extruder_id >= 0);
    return extruder_id < 0 ? -1 : extruder_id;
}

std::map<int32_t, std::vector<LayerRegion>> infill_regions_by_extruder(const LayerIsland &island)
{
    std::map<int32_t, std::vector<LayerRegion>> out;
    for (uint32_t region_idx = 0; region_idx < island.region_count(); ++region_idx) {
        const LayerRegion region = island.region(region_idx);
        out[region_infill_extruder_id(region)].push_back(region);
    }
    return out;
}

bool config_int_value(const Config &config, const char *key, int32_t &out)
{
    const config_option_handle *option = config_get(config.handle(), key);
    if (option == nullptr)
        return false;

    out = ConfigOption(option).get_int();
    return true;
}

bool first_layer_top_surface_has_priority(const Object &object, const bool is_first_layer)
{
    if (!is_first_layer)
        return false;

    // A single exposed first layer is normally both a bottom and a top surface.
    // Without raft layers it should be treated as a top surface, because it is
    // the visible upper skin of the printed object. If the setting is not
    // available in the current config, keep the default bottom-first rule.
    int32_t raft_layers = -1;
    return config_int_value(object.config(), "raft_layers", raft_layers) && raft_layers == 0;
}

std::vector<const layer_region_handle *> region_handles(const std::vector<LayerRegion> &regions)
{
    std::vector<const layer_region_handle *> out;
    out.reserve(regions.size());
    for (const LayerRegion &region : regions)
        out.push_back(region.handle());
    return out;
}

layer_region_island_handle *get_or_create_region_island(const run_ctx_surface_generation &ctx,
                                                        const LayerIsland &island,
                                                        const std::vector<LayerRegion> &regions)
{
    if (ctx.get_or_create_region_island == nullptr)
        return nullptr;

    std::vector<const layer_region_handle *> handles = region_handles(regions);
    const layer_region_handle *const *raw_handles = handles.empty() ? nullptr : handles.data();
    return ctx.get_or_create_region_island(island.handle(), raw_handles, uint32_t(handles.size()));
}

StoredExPolygonCollection clip_infill_areas_to_regions(storage_handle *storage,
                                                       const LayerIsland &island,
                                                       const std::vector<LayerRegion> &regions)
{
    assert(storage != nullptr);

    // Multiple infill extruders inside one island must not share the same fill
    // surfaces. Region raw slices are already non-overlapping, so the split is
    // simply "island infill areas intersected with the union of this extruder's
    // regions".
    StoredExPolygonCollection region_slices(storage);
    for (const LayerRegion &region : regions)
        region_slices.append_copy_from(region.slices());

    ClipperContext clipper(storage);
    ClipperOperand merged_regions = clipper_union(clipper(region_slices.readonly()));
    ClipperOperand clipped = clipper_intersection(clipper(island.infill_areas()), merged_regions);
    return clipped.to_expolygon_collection();
}

StoredExPolygonCollection linked_island_slices(storage_handle *storage,
                                               const std::vector<LayerIsland> &linked_islands)
{
    assert(storage != nullptr);

    StoredExPolygonCollection slices(storage);
    for (const LayerIsland &linked_island : linked_islands)
        slices.push_back(linked_island.slice());
    return slices;
}

StoredExPolygonCollection areas_without_linked_slices(storage_handle *storage,
                                                      const ExPolygonCollection &areas,
                                                      const std::vector<LayerIsland> &linked_islands)
{
    assert(storage != nullptr);

    // Empty overlap lists are meaningful: there is no material on the adjacent
    // layer under/over this island, so every input fill area belongs to the
    // requested surface class.
    if (linked_islands.empty())
        return areas.clone(storage);

    StoredExPolygonCollection slices = linked_island_slices(storage, linked_islands);
    ClipperContext clipper(storage);
    ClipperOperand uncovered = clipper_diff(clipper(areas), clipper(slices.readonly()));
    return uncovered.to_expolygon_collection();
}

StoredExPolygonCollection subtract_areas(storage_handle *storage,
                                         const ExPolygonCollection &subject,
                                         const ExPolygonCollection &clip_areas)
{
    assert(storage != nullptr);

    if (subject.empty())
        return StoredExPolygonCollection(storage);
    if (clip_areas.empty())
        return subject.clone(storage);

    ClipperContext clipper(storage);
    return clipper_diff(clipper(subject), clipper(clip_areas)).to_expolygon_collection();
}

StoredExPolygonCollection occupied_union(storage_handle *storage,
                                         const ExPolygonCollection &first,
                                         const ExPolygonCollection &second)
{
    assert(storage != nullptr);

    StoredExPolygonCollection occupied(storage);
    occupied.append_copy_from(first);
    occupied.append_copy_from(second);
    if (occupied.empty())
        return occupied;

    ClipperContext clipper(storage);
    return clipper_union(clipper(occupied.readonly())).to_expolygon_collection();
}

void append_surface_group(StoredSurfaceCollection &surfaces,
                          const ExPolygonCollection &areas,
                          const raw_surface_type surface_type)
{
    if (!areas.empty())
        surfaces.append(areas, surface_type);
}

StoredSurfaceCollection classify_areas(storage_handle *storage,
                                       const LayerIsland &island,
                                       const ExPolygonCollection &areas,
                                       const bool is_first_layer,
                                       const bool first_layer_top_priority)
{
    assert(storage != nullptr);

    // Bottom and top are detected independently. A thin object can expose the
    // same fill area on both sides, so the priority rule below decides which
    // single Surface type owns the overlap.
    StoredExPolygonCollection raw_bottom =
        areas_without_linked_slices(storage, areas, island.lower_islands());
    StoredExPolygonCollection raw_top =
        areas_without_linked_slices(storage, areas, island.upper_islands());

    StoredExPolygonCollection bottom(storage);
    StoredExPolygonCollection top(storage);
    if (first_layer_top_priority) {
        bottom = subtract_areas(storage, raw_bottom.readonly(), raw_top.readonly());
        top = raw_top.readonly().clone(storage);
    } else {
        bottom = raw_bottom.readonly().clone(storage);
        top = subtract_areas(storage, raw_top.readonly(), raw_bottom.readonly());
    }

    // Everything left after top/bottom classification remains regular sparse
    // infill. The union prevents tiny duplicated borders between top and
    // bottom fragments from being subtracted twice.
    StoredExPolygonCollection occupied = occupied_union(storage, bottom.readonly(), top.readonly());
    StoredExPolygonCollection internal = subtract_areas(storage, areas, occupied.readonly());

    StoredSurfaceCollection surfaces(storage);
    append_surface_group(surfaces, bottom.readonly(), bottom_surface_type(is_first_layer));
    append_surface_group(surfaces, internal.readonly(), k_internal_surface);
    append_surface_group(surfaces, top.readonly(), k_top_surface);
    return surfaces;
}

void set_region_island_surfaces(const run_ctx_surface_generation &ctx,
                                layer_region_island_handle *region_island,
                                StoredSurfaceCollection &surfaces)
{
    if (ctx.set_region_island_fill_surfaces == nullptr || region_island == nullptr)
        return;

    // The callback moves the whole collection into the LayerRegionIsland. The
    // plugin must not read the collection after this call because ownership of
    // its content has been transferred to the host data tree.
    ctx.set_region_island_fill_surfaces(region_island, surfaces.mutable_handle());
}

void build_island_surfaces(const run_ctx_surface_generation &ctx,
                           storage_handle *storage,
                           const Object &object,
                           const LayerIsland &island,
                           const bool is_first_layer)
{
    std::map<int32_t, std::vector<LayerRegion>> grouped_regions = infill_regions_by_extruder(island);
    if (grouped_regions.empty())
        return;

    const bool first_layer_top_priority = first_layer_top_surface_has_priority(object, is_first_layer);
    const bool single_group = grouped_regions.size() == 1;
    for (const auto &[extruder_id, regions] : grouped_regions) {
        (void)extruder_id;
        layer_region_island_handle *region_island = get_or_create_region_island(ctx, island, regions);
        if (region_island == nullptr)
            continue;

        if (single_group) {
            StoredSurfaceCollection surfaces =
                classify_areas(storage, island, island.infill_areas(), is_first_layer, first_layer_top_priority);
            set_region_island_surfaces(ctx, region_island, surfaces);
            continue;
        }

        StoredExPolygonCollection clipped_areas = clip_infill_areas_to_regions(storage, island, regions);
        StoredSurfaceCollection surfaces =
            classify_areas(storage, island, clipped_areas.readonly(), is_first_layer, first_layer_top_priority);
        set_region_island_surfaces(ctx, region_island, surfaces);
    }
}

} // namespace

InitialTypedSurfaceBuilder &
InitialTypedSurfaceBuilder::instance(orchestrator_handle *orch)
{
    static InitialTypedSurfaceBuilder s_instance(orch);
    return s_instance;
}

const char *InitialTypedSurfaceBuilder::id_impl() const noexcept
{
    return k_initial_typed_surface_builder_id;
}

const char *InitialTypedSurfaceBuilder::name_impl() const noexcept
{
    return "Initial typed surface builder";
}

const char *InitialTypedSurfaceBuilder::description_impl() const noexcept
{
    return "Creates initial infill surfaces from perimeter fill areas and classifies them as bottom, internal, or top.";
}

const char *InitialTypedSurfaceBuilder::exclusive_group_impl() const noexcept
{
    return k_surface_generation_group;
}

const char *InitialTypedSurfaceBuilder::exclusive_group_label_impl() const noexcept
{
    return "Surface generation plugin";
}

const char *InitialTypedSurfaceBuilder::exclusive_group_tooltip_impl() const noexcept
{
    return "Choose which active plugin converts perimeter fill areas into infill surfaces.";
}

slicing_step_t InitialTypedSurfaceBuilder::step_impl() const noexcept
{
    return STEP_SURFACE_GENERATION;
}

const char *const *InitialTypedSurfaceBuilder::dependencies_impl() const noexcept
{
    return k_no_dependencies;
}

int32_t InitialTypedSurfaceBuilder::priority_impl() const noexcept
{
    return 0;
}

const char *InitialTypedSurfaceBuilder::progress_message_format_impl() const noexcept
{
    return "Build initial typed surfaces: %u / %u layers";
}

void InitialTypedSurfaceBuilder::setup_run_impl(const plugin_run_context *run_ctx) const
{
    const run_ctx_surface_generation *ctx = plugin_ctx_as_surface_generation(run_ctx);
    if (ctx != nullptr && ctx->object != nullptr) {
        const Object object(ctx->object);
        progress().add_max(object.layer_count());
    }
}

void InitialTypedSurfaceBuilder::run_impl(const plugin_run_context *run_ctx) const
{
    const run_ctx_surface_generation *ctx = plugin_ctx_as_surface_generation(run_ctx);
    if (ctx == nullptr || ctx->object == nullptr || run_ctx == nullptr || run_ctx->plugin_storage == nullptr)
        return;

    const Object object(ctx->object);
    for (uint32_t layer_idx = 0; layer_idx < object.layer_count(); ++layer_idx) {
        throw_if_cancelled(run_ctx);

        const Layer layer = object.layer(layer_idx);
        const bool is_first_layer = layer_idx == 0;
        for (uint32_t island_idx = 0; island_idx < layer.island_count(); ++island_idx)
            build_island_surfaces(*ctx, run_ctx->plugin_storage, object, layer.island(island_idx), is_first_layer);

        progress().increment();
    }
}

void register_initial_typed_surface_builder_plugin(orchestrator_handle *orch)
{
    orchestrator_register_plugin(orch, InitialTypedSurfaceBuilder::instance(orch).c_instance());
}

}}} // namespace slic3r_api::SurfaceGeneration::InitialTypedSurfaceBuilderPlugin

#ifdef INITIAL_TYPED_SURFACE_BUILDER_PLUGIN_DLL
extern "C" void register_plugin(orchestrator_handle *orch)
{
    slic3r_api::SurfaceGeneration::InitialTypedSurfaceBuilderPlugin::register_initial_typed_surface_builder_plugin(orch);
}
#endif // INITIAL_TYPED_SURFACE_BUILDER_PLUGIN_DLL
