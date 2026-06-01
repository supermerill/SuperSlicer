///|/ Copyright (c) SuperSlicer 2026 Durand Remi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/

#include "InfillRegionCompatibilitySplitter.hpp"

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "libslic3r/Api/plugin/c/slic3r_orchestrator.h"
#include "libslic3r/Api/plugin/c/steps/slic3r_step_surface_generation.h"
#include "libslic3r/Api/plugin/cpp/ClipperViews.hpp"
#include "libslic3r/Api/plugin/cpp/DataTreeViews.hpp"
#include "libslic3r/Api/plugin/cpp/RegionSettingsViews.hpp"

namespace slic3r_api { namespace SurfaceGeneration { namespace InfillRegionCompatibilitySplitterPlugin {
namespace {

const char *k_infill_region_compatibility_splitter_id = "surface.infill_region_compatibility_splitter";
const char *k_dependencies[] = { "surface.clean_infill_surfaces", nullptr };

const char *k_infill_extruder_key = "infill_extruder";
const char *k_solid_infill_extruder_key = "solid_infill_extruder";
const char *k_print_extrusion_multiplier_key = "print_extrusion_multiplier";
const char *k_region_gcode_key = "region_gcode";
const char *k_wipe_into_infill_key = "wipe_into_infill";
const char *k_print_first_layer_temperature_key = "print_first_layer_temperature";
const char *k_print_temperature_key = "print_temperature";
const char *k_fill_pattern_key = "fill_pattern";
const char *k_solid_fill_pattern_key = "solid_fill_pattern";
const char *k_top_fill_pattern_key = "top_fill_pattern";
const char *k_bottom_fill_pattern_key = "bottom_fill_pattern";
const char *k_bridge_fill_pattern_key = "bridge_fill_pattern";
const char *k_concentric_pattern_id = "concentric";

const raw_used_config_key k_used_config_keys[] = {
    { k_infill_extruder_key, RAW_CO_INT, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE },
    { k_solid_infill_extruder_key, RAW_CO_INT, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE },
    { k_print_extrusion_multiplier_key, RAW_CO_PERCENT, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE },
    { k_region_gcode_key, RAW_CO_STRING, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE },
    { k_wipe_into_infill_key, RAW_CO_BOOL, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE },
    { k_print_first_layer_temperature_key, RAW_CO_INT, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE },
    { k_print_temperature_key, RAW_CO_INT, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE },
    { k_fill_pattern_key, RAW_CO_ENUM, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE },
    { k_solid_fill_pattern_key, RAW_CO_ENUM, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE },
    { k_top_fill_pattern_key, RAW_CO_ENUM, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE },
    { k_bottom_fill_pattern_key, RAW_CO_ENUM, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE },
    { k_bridge_fill_pattern_key, RAW_CO_ENUM, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE }
};

struct RegionKey
{
    std::vector<const layer_region_handle *> handles;
    int32_t extruder_id = -1;

    bool operator==(const RegionKey &rhs) const
    {
        return handles == rhs.handles && extruder_id == rhs.extruder_id;
    }
};

struct PendingSurfaceGroup
{
    RegionKey regions;
    raw_extrusion_role role;
    StoredSurfaceCollection surfaces;
};

bool is_sparse_surface(const Surface &surface)
{
    return surface_type_is_sparse(surface.type());
}

bool is_solid_surface(const Surface &surface)
{
    return surface_type_is_solid(surface.type());
}

raw_extrusion_role raw_role_for_surface(const Surface &surface)
{
    // The destination LayerRegionIsland is keyed by extruder, and the host
    // resolves that extruder from an extrusion role. Keep this mapping in sync
    // with the infill generator: bridge and solid surfaces use the solid-infill
    // extruder, while sparse surfaces use the sparse-infill extruder.
    if (surface.has_flag(RAW_SURFACE_TYPE_MOD_BRIDGE))
        return surface.has_flag(RAW_SURFACE_TYPE_POS_BOTTOM) ? RAW_EXTRUSION_ROLE_BRIDGE_INFILL :
                                                               RAW_EXTRUSION_ROLE_INTERNAL_BRIDGE_INFILL;
    if (is_solid_surface(surface))
        return surface.has_flag(RAW_SURFACE_TYPE_POS_TOP) ? RAW_EXTRUSION_ROLE_TOP_SOLID_INFILL :
                                                            RAW_EXTRUSION_ROLE_SOLID_INFILL;
    return RAW_EXTRUSION_ROLE_INTERNAL_INFILL;
}

const char *extruder_key_for_role(const raw_extrusion_role role)
{
    if ((role & RAW_EXTRUSION_ROLE_INFILL) == 0 && role != RAW_EXTRUSION_ROLE_GAP_FILL)
        return nullptr;
    if ((role & RAW_EXTRUSION_ROLE_SOLID) != 0 ||
        (role & RAW_EXTRUSION_ROLE_BRIDGE) != 0 ||
        (role & RAW_EXTRUSION_ROLE_IRONING) != 0)
        return k_solid_infill_extruder_key;
    return k_infill_extruder_key;
}

int32_t region_extruder_id_for_role(const LayerRegion &region, const raw_extrusion_role role)
{
    const char *key = extruder_key_for_role(role);
    if (key == nullptr || !region.print_region().config().has(key))
        return -1;

    const int32_t extruder = region.print_region().config().get(key).get_int();
    return extruder <= 0 ? -1 : extruder - 1;
}

const char *pattern_key_for_surface(const Surface &surface)
{
    // The infill generator resolves the effective pattern from the surface
    // role. Use the same priority here so compatibility splitting matches the
    // recipe that will be used later.
    if (surface.has_flag(RAW_SURFACE_TYPE_MOD_BRIDGE))
        return k_bridge_fill_pattern_key;
    if (surface.has_flag(RAW_SURFACE_TYPE_POS_TOP))
        return k_top_fill_pattern_key;
    if (surface.has_flag(RAW_SURFACE_TYPE_POS_BOTTOM))
        return k_bottom_fill_pattern_key;
    if (is_solid_surface(surface))
        return k_solid_fill_pattern_key;
    if (is_sparse_surface(surface))
        return k_fill_pattern_key;
    return nullptr;
}

std::string normalized_serialized_value(const ConfigOption &option)
{
    std::string value = option.serialize();
    if (!value.empty() && value.front() == '!')
        value.erase(value.begin());
    return value;
}

bool region_uses_concentric_pattern(const LayerRegion &region, const char *pattern_key)
{
    if (pattern_key == nullptr || !region.print_region().config().has(pattern_key))
        return false;
    return normalized_serialized_value(region.print_region().config().get(pattern_key)) == k_concentric_pattern_id;
}

bool any_region_uses_concentric_pattern(const std::vector<LayerRegion> &regions, const Surface &surface)
{
    const char *pattern_key = pattern_key_for_surface(surface);
    for (const LayerRegion &region : regions)
        if (region_uses_concentric_pattern(region, pattern_key))
            return true;
    return false;
}

void add_unique_key(RegionSettings::OptionKeyGroup &keys, const char *key)
{
    if (key != nullptr &&
        std::find(keys.begin(), keys.end(), std::string(key)) == keys.end())
        keys.emplace_back(key);
}

void add_key_if_present(RegionSettings::OptionKeyGroup &keys, const Config &config, const char *key)
{
    if (key != nullptr && config.has(key))
        add_unique_key(keys, key);
}

void append_selected_perimeter_keys(orchestrator_handle *orchestrator,
                                    const Config &print_config,
                                    const Config &region_config,
                                    RegionSettings::OptionKeyGroup &keys)
{
    const int32_t key_count =
        orchestrator_selected_plugin_used_config_keys(orchestrator, print_config.handle(), STEP_PERIMETER, nullptr);
    if (key_count <= 0)
        return;

    std::vector<raw_used_config_key> selected_keys(static_cast<size_t>(key_count));
    orchestrator_selected_plugin_used_config_keys(orchestrator,
                                                  print_config.handle(),
                                                  STEP_PERIMETER,
                                                  selected_keys.data());

    for (const raw_used_config_key &used_key : selected_keys) {
        // Only region-visible keys can split LayerRegionIsland surfaces. A
        // project-level selector may still decide which perimeter plugin runs,
        // but it has one value for the whole print and therefore cannot require
        // a geometric split between regions.
        if (used_key.key == nullptr)
            continue;
        if (used_key.container_type != RAW_CONTAINER_TYPE_NONE &&
            used_key.container_type != RAW_CONTAINER_TYPE_REGION)
            continue;
        if (region_config.has(used_key.key))
            add_unique_key(keys, used_key.key);
    }
}

RegionSettings::OptionKeyGroup option_group_for_surface(orchestrator_handle *orchestrator,
                                                        const Config &print_config,
                                                        const std::vector<LayerRegion> &regions,
                                                        const Surface &surface)
{
    RegionSettings::OptionKeyGroup keys;
    if (regions.empty())
        return keys;

    const Config first_region_config = regions.front().print_region().config();

    add_key_if_present(keys, first_region_config, k_print_extrusion_multiplier_key);
    add_key_if_present(keys, first_region_config, k_region_gcode_key);
    add_key_if_present(keys, first_region_config, k_wipe_into_infill_key);
    add_key_if_present(keys, first_region_config, k_print_first_layer_temperature_key);
    add_key_if_present(keys, first_region_config, k_print_temperature_key);

    if (is_sparse_surface(surface))
        add_key_if_present(keys, first_region_config, k_infill_extruder_key);
    if (is_solid_surface(surface))
        add_key_if_present(keys, first_region_config, k_solid_infill_extruder_key);

    const char *pattern_key = pattern_key_for_surface(surface);
    add_key_if_present(keys, first_region_config, pattern_key);
    if (any_region_uses_concentric_pattern(regions, surface))
        append_selected_perimeter_keys(orchestrator, print_config, first_region_config, keys);

    return keys;
}

bool key_from_regions(const std::vector<LayerRegion> &regions,
                      const raw_extrusion_role role,
                      RegionKey &key)
{
    key.handles.reserve(regions.size());
    for (const LayerRegion &region : regions) {
        const int32_t extruder_id = region_extruder_id_for_role(region, role);
        if (extruder_id < 0)
            return false;
        if (key.extruder_id < 0)
            key.extruder_id = extruder_id;
        else if (key.extruder_id != extruder_id)
            return false;
        key.handles.push_back(region.handle());
    }

    std::sort(key.handles.begin(), key.handles.end());
    key.handles.erase(std::unique(key.handles.begin(), key.handles.end()), key.handles.end());
    return key.extruder_id >= 0 && !key.handles.empty();
}

StoredSurfaceCollection &surfaces_for_region_key(std::vector<PendingSurfaceGroup> &groups,
                                                 storage_handle *storage,
                                                 RegionKey key,
                                                 const raw_extrusion_role role)
{
    for (PendingSurfaceGroup &group : groups)
        if (group.regions == key)
            return group.surfaces;

    groups.push_back(PendingSurfaceGroup{std::move(key), role, StoredSurfaceCollection(storage)});
    return groups.back().surfaces;
}

void append_surface_like(const run_ctx_surface_generation &ctx,
                         StoredSurfaceCollection &dst,
                         const Surface &source,
                         const ExPolygonCollection &areas)
{
    // The splitter only changes geometry. The host callback preserves every
    // other Surface field, including metadata that the C++ view does not expose.
    assert(ctx.append_surface_like != nullptr);
    if (!areas.empty() && ctx.append_surface_like != nullptr)
        ctx.append_surface_like(dst.mutable_handle(), source.handle(), areas.handle());
}

void append_surface_piece(StoredSurfaceCollection &dst,
                          const run_ctx_surface_generation &ctx,
                          const Surface &surface,
                          const RegionSettingsClip &clip)
{
    // Preserve every non-geometry field by appending "like" the source surface.
    // The only thing that changes is the ExPolygon produced by the split.
    StoredExPolygonCollection clipped = clip.intersections(surface.expolygon());
    append_surface_like(ctx, dst, surface, clipped.readonly());
}

void append_unsplit_surface(std::vector<PendingSurfaceGroup> &groups,
                            storage_handle *storage,
                            const run_ctx_surface_generation &ctx,
                            const std::vector<LayerRegion> &regions,
                            const Surface &surface)
{
    RegionKey region_key;
    const raw_extrusion_role role = raw_role_for_surface(surface);
    if (!key_from_regions(regions, role, region_key))
        return;

    StoredSurfaceCollection &surfaces = surfaces_for_region_key(groups, storage, std::move(region_key), role);
    StoredExPolygonCollection single(storage);
    single.push_back(surface.expolygon());
    append_surface_like(ctx, surfaces, surface, single.readonly());
}

void split_surface_by_region_settings(orchestrator_handle *orchestrator,
                                      storage_handle *storage,
                                      const run_ctx_surface_generation &ctx,
                                      const Config &print_config,
                                      const std::vector<LayerRegion> &regions,
                                      const Surface &surface,
                                      std::vector<PendingSurfaceGroup> &groups)
{
    RegionSettings::OptionKeyGroup option_group =
        option_group_for_surface(orchestrator, print_config, regions, surface);
    if (option_group.empty()) {
        append_unsplit_surface(groups, storage, ctx, regions, surface);
        return;
    }

    const Config default_config = regions.front().print_region().config();
    std::vector<RegionSettings::OptionKeyGroup> option_groups;
    option_groups.push_back(option_group);
    RegionSettings settings(storage, default_config, std::move(option_groups));
    for (const LayerRegion &region : regions)
        settings.add_region(region);
    settings.segregate(surface.expolygon());

    const char *primary_key = option_group.front().c_str();
    if (!settings.has_many_config(primary_key)) {
        append_unsplit_surface(groups, storage, ctx, regions, surface);
        return;
    }

    const RegionSettings::AreaMap &areas = settings.get_areas(primary_key);
    for (const auto &[setting_value, setting_clip] : areas) {
        const std::vector<LayerRegion> &target_regions = settings.get_regions(primary_key, setting_value);
        if (target_regions.empty())
            continue;

        RegionKey target_key;
        const raw_extrusion_role role = raw_role_for_surface(surface);
        if (!key_from_regions(target_regions, role, target_key))
            continue;

        StoredSurfaceCollection &surfaces = surfaces_for_region_key(groups, storage, std::move(target_key), role);
        append_surface_piece(surfaces, ctx, surface, setting_clip);
    }
}

bool has_same_single_group(const std::vector<PendingSurfaceGroup> &groups, const RegionKey &original_region_key)
{
    return groups.size() == 1 && groups.front().regions == original_region_key;
}

void publish_split_groups(const run_ctx_surface_generation &ctx,
                          const LayerIsland &island,
                          const LayerRegionIsland &original_region_island,
                          std::vector<PendingSurfaceGroup> &groups)
{
    if (ctx.get_or_create_region_island == nullptr || ctx.set_region_island_fill_surfaces == nullptr)
        return;

    // Clear the old group first. Any newly created compatible group gets a full
    // replacement collection below. If one target uses the original region set,
    // get_or_create_region_island() simply returns this cleared group.
    ctx.set_region_island_fill_surfaces(
        const_cast<layer_region_island_handle *>(original_region_island.handle()),
        nullptr);

    for (PendingSurfaceGroup &group : groups) {
        if (group.surfaces.empty())
            continue;

        const layer_region_handle *const *handles =
            group.regions.handles.empty() ? nullptr : group.regions.handles.data();
        layer_region_island_handle *target =
            ctx.get_or_create_region_island(
                island.handle(), handles, uint32_t(group.regions.handles.size()), group.role);
        if (target != nullptr)
            ctx.set_region_island_fill_surfaces(target, group.surfaces.mutable_handle());
    }
}

void split_region_island_if_needed(orchestrator_handle *orchestrator,
                                   const run_ctx_surface_generation &ctx,
                                   storage_handle *storage,
                                   const Config &print_config,
                                   const LayerIsland &island,
                                   const LayerRegionIsland &region_island)
{
    const SurfaceCollection input_surfaces = region_island.fill_surfaces_collection();
    if (input_surfaces.empty())
        return;

    const std::vector<LayerRegion> regions = region_island.regions();
    if (regions.empty())
        return;

    RegionKey original_region_key;
    original_region_key.extruder_id = region_island.extruder_id();
    for (const LayerRegion &region : regions)
        original_region_key.handles.push_back(region.handle());
    std::sort(original_region_key.handles.begin(), original_region_key.handles.end());
    original_region_key.handles.erase(
        std::unique(original_region_key.handles.begin(), original_region_key.handles.end()),
        original_region_key.handles.end());

    std::vector<PendingSurfaceGroup> groups;
    for (const Surface surface : input_surfaces)
        split_surface_by_region_settings(orchestrator,
                                         storage,
                                         ctx,
                                         print_config,
                                         regions,
                                         surface,
                                         groups);

    // If every surface still belongs to the same region set, avoid a needless
    // whole-collection replacement. This keeps pointer churn low for the common
    // case where regions are already compatible for infill.
    if (has_same_single_group(groups, original_region_key))
        return;

    publish_split_groups(ctx, island, region_island, groups);
}

void process_layer(orchestrator_handle *orchestrator,
                   const run_ctx_surface_generation &ctx,
                   storage_handle *storage,
                   const Config &print_config,
                   const Object &object,
                   const uint32_t layer_idx)
{
    const Layer layer = object.layer(layer_idx);
    for (uint32_t island_idx = 0; island_idx < layer.island_count(); ++island_idx) {
        const LayerIsland island = layer.island(island_idx);

        // Snapshot existing groups before publishing any split result. New
        // groups created by this plugin are final outputs for this layer, not
        // additional inputs that should be split again in the same pass.
        std::vector<LayerRegionIsland> region_islands;
        region_islands.reserve(island.region_island_count());
        for (uint32_t region_island_idx = 0; region_island_idx < island.region_island_count(); ++region_island_idx)
            region_islands.emplace_back(island.region_island(region_island_idx));

        for (const LayerRegionIsland &region_island : region_islands)
            split_region_island_if_needed(orchestrator, ctx, storage, print_config, island, region_island);
    }
}

} // namespace

InfillRegionCompatibilitySplitter &
InfillRegionCompatibilitySplitter::instance(orchestrator_handle *orch)
{
    static InfillRegionCompatibilitySplitter s_instance(orch);
    return s_instance;
}

const char *InfillRegionCompatibilitySplitter::id_impl() const noexcept
{
    return k_infill_region_compatibility_splitter_id;
}

const char *InfillRegionCompatibilitySplitter::name_impl() const noexcept
{
    return "Infill region compatibility splitter";
}

const char *InfillRegionCompatibilitySplitter::description_impl() const noexcept
{
    return "Splits final fill surfaces by the region settings that make infill generation incompatible.";
}

slicing_step_t InfillRegionCompatibilitySplitter::step_impl() const noexcept
{
    return STEP_SURFACE_GENERATION;
}

const char *const *InfillRegionCompatibilitySplitter::dependencies_impl() const noexcept
{
    return k_dependencies;
}

int32_t InfillRegionCompatibilitySplitter::priority_impl() const noexcept
{
    return 200;
}

int32_t InfillRegionCompatibilitySplitter::used_config_keys(raw_used_config_key *keys) const noexcept
{
    if (keys != nullptr)
        for (uint32_t idx = 0; idx < sizeof(k_used_config_keys) / sizeof(k_used_config_keys[0]); ++idx)
            keys[idx] = k_used_config_keys[idx];
    return int32_t(sizeof(k_used_config_keys) / sizeof(k_used_config_keys[0]));
}

const char *InfillRegionCompatibilitySplitter::progress_message_format_impl() const noexcept
{
    return "Split infill surface regions: %u / %u layers";
}

void InfillRegionCompatibilitySplitter::setup_run_impl(const plugin_run_context *run_ctx) const
{
    const run_ctx_surface_generation *ctx = plugin_ctx_as_surface_generation(run_ctx);
    if (ctx != nullptr && ctx->object != nullptr) {
        const Object object(ctx->object);
        progress().add_max(object.layer_count());
    }
}

void InfillRegionCompatibilitySplitter::run_impl(const plugin_run_context *run_ctx) const
{
    const run_ctx_surface_generation *ctx = plugin_ctx_as_surface_generation(run_ctx);
    if (ctx == nullptr || ctx->object == nullptr || ctx->print == nullptr ||
        run_ctx == nullptr || run_ctx->plugin_storage == nullptr)
        return;

    const Object object(ctx->object);
    const Print print(ctx->print);
    const Config print_config = print.config();
    for (uint32_t layer_idx = 0; layer_idx < object.layer_count(); ++layer_idx) {
        throw_if_cancelled(run_ctx);
        process_layer(m_orchestrator, *ctx, run_ctx->plugin_storage, print_config, object, layer_idx);
        progress().increment();
    }
}

void register_infill_region_compatibility_splitter_plugin(orchestrator_handle *orch)
{
    orchestrator_register_plugin(orch, InfillRegionCompatibilitySplitter::instance(orch).c_instance());
}

}}} // namespace slic3r_api::SurfaceGeneration::InfillRegionCompatibilitySplitterPlugin

#ifdef INFILL_REGION_COMPATIBILITY_SPLITTER_PLUGIN_DLL
extern "C" void register_plugin(orchestrator_handle *orch)
{
    slic3r_api::SurfaceGeneration::InfillRegionCompatibilitySplitterPlugin::
        register_infill_region_compatibility_splitter_plugin(orch);
}
#endif // INFILL_REGION_COMPATIBILITY_SPLITTER_PLUGIN_DLL
