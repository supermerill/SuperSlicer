
#include "StepPostInfillGeneration.hpp"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <vector>

#include "libslic3r/ExtrusionEntity.hpp"
#include "libslic3r/Layer.hpp"
#include "libslic3r/LayerRegion.hpp"
#include "libslic3r/Api/host/Plugin.hpp"
#include "libslic3r/Api/plugin/c/steps/slic3r_step_post_infill.h"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintObject.hpp"
#include "libslic3r/PrintRegion.hpp"
#include "libslic3r/Steps/StepRunner.hpp"

namespace Slic3r::Steps::StepPostInfillGeneration {
namespace {

LayerRegionIsland *to_layer_region_island(const layer_region_island_handle *handle)
{
    return const_cast<LayerRegionIsland *>(reinterpret_cast<const LayerRegionIsland *>(handle));
}

LayerSliceIsland *to_layer_island(const layer_island_handle *handle)
{
    return const_cast<LayerSliceIsland *>(reinterpret_cast<const LayerSliceIsland *>(handle));
}

const LayerRegion *to_layer_region(const layer_region_handle *handle)
{
    return reinterpret_cast<const LayerRegion *>(handle);
}

bool role_is_post_infill_owned(raw_extrusion_role role)
{
    // This step runs after infill generation. A plugin here may only edit
    // extrusion trees that were produced for infill-related work. Other
    // extrusion classes are handled earlier or later in the pipeline, so we
    // reject them here instead of giving a plugin a pointer to the wrong bucket.
    return (role & RAW_EXTRUSION_ROLE_INFILL) != 0 ||
           (role & RAW_EXTRUSION_ROLE_IRONING) != 0 ||
           role == RAW_EXTRUSION_ROLE_GAP_FILL;
}

ExtrusionRole bucket_role_from_raw(raw_extrusion_role role)
{
    // A LayerRegionIsland does not store one root per exact extrusion role.
    // Instead it stores a few large roots: all sparse/solid/bridge infill
    // paths share INFILLS, ironing paths share IRONINGS, and narrow residual
    // paths share GAP_FILLS. The exact role is still stored on each child path.
    if ((role & RAW_EXTRUSION_ROLE_IRONING) != 0)
        return LayerRegionIsland::IRONINGS;
    if (role == RAW_EXTRUSION_ROLE_GAP_FILL)
        return LayerRegionIsland::GAP_FILLS;
    return LayerRegionIsland::INFILLS;
}

LayerRegionSetCPtrs region_set_from_handles(const layer_region_handle *const *region_handles,
                                             uint32_t region_count,
                                             const LayerSliceIsland &island)
{
    // A post-infill plugin usually wants to publish into the same region group
    // as an existing LayerRegionIsland. Passing no explicit list means "all
    // regions of this island", matching the surface-generation step contract.
    LayerRegionSetCPtrs regions;
    if (region_handles == nullptr || region_count == 0)
        return island.regions();

    for (uint32_t idx = 0; idx < region_count; ++idx) {
        const LayerRegion *region = to_layer_region(region_handles[idx]);
        if (region != nullptr)
            regions.insert(region);
    }
    return regions;
}

bool region_extruder_id_for_role(const LayerRegion &region,
                                 const raw_extrusion_role role,
                                 uint16_t &extruder_id)
{
    // STEP_POST_INFILL may move generated infill between region groups. The
    // output role decides which region extruder setting participates in the
    // LayerRegionIsland key, matching STEP_SURFACE_GENERATION.
    int extruder = 0;
    if ((role & RAW_EXTRUSION_ROLE_INFILL) != 0) {
        if ((role & RAW_EXTRUSION_ROLE_SOLID) != 0 ||
            (role & RAW_EXTRUSION_ROLE_BRIDGE) != 0 ||
            (role & RAW_EXTRUSION_ROLE_IRONING) != 0)
            extruder = region.region().config().solid_infill_extruder.value;
        else
            extruder = region.region().config().infill_extruder.value;
    } else if (role == RAW_EXTRUSION_ROLE_GAP_FILL) {
        extruder = region.region().config().infill_extruder.value;
    } else {
        return false;
    }

    if (extruder <= 0)
        return false;

    extruder_id = uint16_t(extruder - 1);
    return true;
}

bool unique_extruder_id_for_role(const LayerRegionSetCPtrs &regions,
                                 const raw_extrusion_role role,
                                 uint16_t &extruder_id)
{
    // A mixed-extruder group cannot be represented by a single
    // LayerRegionIsland. Returning false makes the C callback fail with NULL,
    // which tells the plugin to split the output before publishing it.
    bool has_extruder = false;
    for (const LayerRegion *region : regions) {
        if (region == nullptr)
            continue;

        uint16_t region_extruder_id = uint16_t(-1);
        if (!region_extruder_id_for_role(*region, role, region_extruder_id))
            return false;

        if (!has_extruder) {
            extruder_id = region_extruder_id;
            has_extruder = true;
            continue;
        }

        if (extruder_id != region_extruder_id)
            return false;
    }

    return has_extruder;
}

layer_region_island_handle *get_or_create_region_island_callback(
    const layer_island_handle *island_handle,
    const layer_region_handle *const *region_handles,
    uint32_t region_count,
    raw_extrusion_role role)
{
    // This is the only topology-writing callback for STEP_POST_INFILL. It lets
    // a plugin request a destination LayerRegionIsland for a region group, but
    // keeps the actual island geometry and slice data read-only.
    LayerSliceIsland *island = to_layer_island(island_handle);
    if (island == nullptr)
        return nullptr;

    LayerRegionSetCPtrs regions = region_set_from_handles(region_handles, region_count, *island);
    if (regions.empty())
        return nullptr;

    uint16_t extruder_id = uint16_t(-1);
    if (!unique_extruder_id_for_role(regions, role, extruder_id))
        return nullptr;

    LayerRegionIsland &region_island = island->get_or_add_region_island(regions, extruder_id);
    return reinterpret_cast<layer_region_island_handle *>(&region_island);
}

extrusion_entity_handle *get_region_island_mutable_extrusion_callback(
    const layer_region_island_handle *region_island_handle,
    raw_extrusion_role role)
{
    LayerRegionIsland *region_island = to_layer_region_island(region_island_handle);
    if (region_island == nullptr || !role_is_post_infill_owned(role))
        return nullptr;

    // The returned object is the host-owned root collection for this bucket.
    // It may be empty when the plugin is about to append the first generated
    // child subtree. Empty roots are removed by the step cleanup after the
    // plugin returns, so a harmless probe does not permanently dirty the tree.
    const ExtrusionRole bucket_role = bucket_role_from_raw(role);
    ExtrusionEntityCollection &collection = region_island->mutable_extrusion(bucket_role);
    return reinterpret_cast<extrusion_entity_handle *>(&collection);
}

void remove_empty_region_islands(Print &print)
{
    // Plugins may ask for a destination LayerRegionIsland, then decide that no
    // output has to be published there. The host removes groups that have no
    // fill surfaces and no real extrusion after each plugin finishes, when no
    // plugin still holds borrowed handles from the current callback.
    for (PrintObject &object : print.objects())
        for (Layer &layer : object.layers())
            for (LayerSliceIsland &island : layer.islands()) {
                LayerRegionIslandUPtrs &region_islands = island.mutable_regions_islands();
                for (LayerRegionIslandUPtr &region_island : region_islands)
                    if (region_island != nullptr)
                        region_island->remove_empty_extrusions();
                region_islands.erase(
                    std::remove_if(region_islands.begin(),
                                   region_islands.end(),
                                   [](const LayerRegionIslandUPtr &region_island) {
                                       return region_island != nullptr &&
                                              region_island->fill_surfaces().empty() &&
                                              !region_island->has_extrusions();
                                   }),
                    region_islands.end());
            }
}

} // namespace

void clean_and_prepare(Print &) {}

bool validate_pre(const Print &, std::string *)
{
    return true;
}

bool validate_post(const Print &, std::string *)
{
    return true;
}

void run_step(Orchestrator &orchestrator, Print &print)
{
    // Several post-infill plugins can run one after another. This is useful for
    // small finishing passes: one plugin may add residual gap fill, another may
    // prepare ironing, and another may simplify or tag the generated infill.
    // They all receive the same object-level payload and run in priority order.
    std::vector<Plugin *> plugins =
        selected_or_active_plugins_for_step(orchestrator, STEP_POST_INFILL, &print.full_print_config());
    for (Plugin *plugin : plugins) {
        if (plugin == nullptr)
            continue;

        Detail::run_object_step_plugin(
            orchestrator,
            print,
            STEP_POST_INFILL,
            *plugin,
            print.objects().size(),
            [&print](const size_t object_idx) {
            run_ctx_post_infill_generation context = {};
            context.print = reinterpret_cast<const print_handle *>(&print);
            context.object = reinterpret_cast<const object_handle *>(&print.object(object_idx));
            context.get_or_create_region_island = &get_or_create_region_island_callback;
            context.get_region_island_mutable_extrusion = &get_region_island_mutable_extrusion_callback;
            return context;
        });
        remove_empty_region_islands(print);
    }
}

} // namespace Slic3r::Steps::StepPostInfillGeneration
