///|/ Copyright (c) SuperSlicer 2026 Durand Remi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/

#include "StepSurfaceGeneration.hpp"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <sstream>
#include <utility>

#include "libslic3r/Api/host/Orchestrator.hpp"
#include "libslic3r/Api/host/Plugin.hpp"
#include "libslic3r/Api/plugin/c/steps/slic3r_step_surface_generation.h"
#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/DataTreeFwd.hpp"
#include "libslic3r/ExPolygon.hpp"
#include "libslic3r/Layer.hpp"
#include "libslic3r/LayerRegion.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintObject.hpp"
#include "libslic3r/PrintRegion.hpp"
#include "libslic3r/Steps/StepPipeline.hpp"
#include "libslic3r/Surface.hpp"
#include "libslic3r/SurfaceCollection.hpp"

#include "StepRunner.hpp"

namespace Slic3r::Steps::StepSurfaceGeneration {
namespace {

LayerRegionIsland *to_layer_region_island(layer_region_island_handle *handle)
{
    return reinterpret_cast<LayerRegionIsland *>(handle);
}

const Surface *to_surface(const surface_handle *handle)
{
    return reinterpret_cast<const Surface *>(handle);
}

SurfaceCollection *to_surface_collection(surface_collection_handle *handle)
{
    return reinterpret_cast<SurfaceCollection *>(handle);
}

const ExPolygons *to_expolygons(const expolygon_collection_handle *handle)
{
    return reinterpret_cast<const ExPolygons *>(handle);
}

double area_sum(const ExPolygons &areas)
{
    double out = 0.;
    for (const ExPolygon &area : areas)
        out += std::abs(area.area());
    return out;
}

double surface_area_sum(const ExPolygons &surfaces)
{
    double out = 0.;
    for (const ExPolygon &surface : surfaces)
        out += std::abs(surface.area());
    return out;
}

double validation_area_tolerance()
{
    return double(SCALED_EPSILON) * double(SCALED_EPSILON);
}

void append_island_surfaces(ExPolygons &out, const LayerSliceIsland &island)
{
    // Surface-generation plugins may split an island into multiple
    // LayerRegionIsland groups. The post-condition is island-wide: infill
    // later consumes the union of those groups, so overlaps between two groups
    // are just as invalid as overlaps inside one group.
    for (const LayerRegionIsland &region_island : island.regions_islands())
        for (const Surface &surface : region_island.fill_surfaces())
            if (!surface.empty())
                out.push_back(surface.expolygon);
}

bool validate_island_surface_partition(const LayerSliceIsland &island,
                                       const size_t object_idx,
                                       const size_t layer_idx,
                                       const size_t island_idx,
                                       std::string *error)
{
    ExPolygons surfaces;
    append_island_surfaces(surfaces, island);

    const double tolerance = validation_area_tolerance();
    const double raw_surface_area = surface_area_sum(surfaces);
    const ExPolygons surface_union = union_ex(surfaces);
    const double union_area = area_sum(surface_union);

    // If surfaces overlap, the sum of their individual areas becomes larger
    // than their union. Boundary-only contact may create tiny numerical dust;
    // the epsilon-squared tolerance accepts only that dust, not real overlap.
    if (raw_surface_area - union_area > tolerance) {
        if (error != nullptr) {
            std::ostringstream stream;
            stream << "Surface-generation post-condition failed for object " << object_idx
                   << ", layer " << layer_idx
                   << ", island " << island_idx
                   << ": fill surfaces overlap by area " << raw_surface_area - union_area
                   << " scaled^2, tolerance " << tolerance << ".";
            *error = stream.str();
        }
        return false;
    }

    const ExPolygons missing = diff_ex(island.infill_areas(), surface_union);
    const ExPolygons extra = diff_ex(surface_union, island.infill_areas());
    const double missing_area = area_sum(missing);
    const double extra_area = area_sum(extra);

    // The surface partition should cover exactly the fillable area produced by
    // perimeter generation. Missing area would leave holes for infill, while
    // extra area would make infill escape the island or reuse perimeter space.
    if (missing_area > tolerance || extra_area > tolerance) {
        if (error != nullptr) {
            std::ostringstream stream;
            stream << "Surface-generation post-condition failed for object " << object_idx
                   << ", layer " << layer_idx
                   << ", island " << island_idx
                   << ": fill surfaces do not match infill_areas. Missing area "
                   << missing_area << " scaled^2, extra area " << extra_area
                   << " scaled^2, tolerance " << tolerance << ".";
            *error = stream.str();
        }
        return false;
    }

    return true;
}

int32_t set_region_island_fill_surfaces_callback(layer_region_island_handle *region_island_handle,
                                                 surface_collection_handle *surfaces_handle)
{
    // Replace the complete fill-surface collection. A whole-collection move is
    // easier to reason about than incremental append/remove callbacks and keeps
    // surface ownership on the host side of the C API boundary.
    LayerRegionIsland *region_island = to_layer_region_island(region_island_handle);
    if (region_island == nullptr)
        return 0;

    SurfaceCollection &surfaces = region_island->set_fill_surfaces();
    if (surfaces_handle == nullptr)
        surfaces.clear();
    else
        surfaces.set(std::move(*to_surface_collection(surfaces_handle)));
    return 1;
}

void append_surface_like_callback(surface_collection_handle *dst_handle,
                                  const surface_handle *source_handle,
                                  const expolygon_collection_handle *areas_handle)
{
    // A Surface contains more than its ExPolygon and type bitmask. When a
    // plugin splits an existing Surface, this callback lets the host preserve
    // all metadata while replacing only the geometry pieces.
    if (dst_handle != nullptr && source_handle != nullptr && areas_handle != nullptr)
        to_surface_collection(dst_handle)->append(*to_expolygons(areas_handle), *to_surface(source_handle));
}

void remove_empty_region_islands(Print &print)
{
    // Surface plugins are allowed to split one LayerRegionIsland into several
    // more specific groups. They clear the old group through the API, but they
    // cannot erase it immediately because that would invalidate handles while a
    // plugin is still traversing the data tree. The host removes empty groups
    // only after a plugin run has fully returned.
    for (PrintObject &object : print.objects())
        for (Layer &layer : object.layers())
            for (LayerSliceIsland &island : layer.islands()) {
                LayerRegionIslandUPtrs &region_islands = island.mutable_regions_islands();
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

void clean_and_prepare(Print &print)
{
    // Surface generation owns only LayerRegionIsland fill surfaces. It must
    // leave deprecated LayerRegion fill caches untouched, because island-level
    // surfaces are the data passed to the new infill pipeline.
    for (PrintObject &object : print.objects())
        for (Layer &layer : object.layers())
            for (LayerSliceIsland &island : layer.islands())
                for (LayerRegionIsland &region_island : island.regions_islands())
                    region_island.set_fill_surfaces().clear();
}

bool validate_pre(const Print &, std::string *)
{
    return true;
}

bool validate_post(const Print &print, std::string *error)
{
    for (size_t object_idx = 0; object_idx < print.objects().size(); ++object_idx) {
        const PrintObject &object = print.object(object_idx);
        for (size_t layer_idx = 0; layer_idx < object.layer_count(); ++layer_idx) {
            const Layer &layer = object.layer(layer_idx);
            for (size_t island_idx = 0; island_idx < layer.islands().size(); ++island_idx)
                if (!validate_island_surface_partition(layer.island(island_idx),
                                                       object_idx,
                                                       layer_idx,
                                                       island_idx,
                                                       error))
                    return false;
        }
    }
    return true;
}

void run_step(Orchestrator &orchestrator, Print &print)
{
    // Surface generation is a small pipeline. One plugin usually creates the
    // initial surfaces from perimeter fill areas, and later plugins refine their
    // type or geometry before infill consumes them.
    std::vector<Plugin *> plugins =
        selected_or_active_plugins_for_step(orchestrator, STEP_SURFACE_GENERATION, &print.full_print_config());
    for (Plugin *plugin : plugins) {
        if (plugin == nullptr)
            continue;

        Detail::run_object_step_plugin(
            orchestrator,
            print,
            STEP_SURFACE_GENERATION,
            *plugin,
            print.objects().size(),
            [&print](const size_t object_idx) {
            run_ctx_surface_generation payload = {};
            payload.print = reinterpret_cast<const print_handle *>(&print);
            payload.object = reinterpret_cast<const object_handle *>(&print.object(object_idx));
            payload.set_region_island_fill_surfaces = &set_region_island_fill_surfaces_callback;
            payload.append_surface_like = &append_surface_like_callback;
            return payload;
        });
        remove_empty_region_islands(print);
    }
}

} // namespace Slic3r::Steps::StepSurfaceGeneration
