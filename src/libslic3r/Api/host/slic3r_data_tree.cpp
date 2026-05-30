///|/ Copyright (c) SuperSlicer 2026 Durand Rémi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/
#include <string>
#include <utility>
#include <vector>

#include "libslic3r/Api/host/ApiHostUtils.hpp"
#include "libslic3r/Api/host/Orchestrator.hpp"
#include "libslic3r/Api/internal/LayerIslandAccess.hpp"
#include "libslic3r/Api/plugin/c/slic3r_data_tree.h"
#include "libslic3r/Api/plugin/c/slic3r_orchestrator.h"
#include "libslic3r/ConfigDef.hpp"
#include "libslic3r/ExPolygon.hpp"
#include "libslic3r/ExtrusionEntityCollection.hpp"
#include "libslic3r/ExtrusionRole.hpp"
#include "libslic3r/Flow.hpp"
#include "libslic3r/Layer.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintObject.hpp"
#include "libslic3r/PrintRegion.hpp"
#include "libslic3r/PluginProperty.hpp"
#include "libslic3r/Surface.hpp"
#include "libslic3r/SurfaceCollection.hpp"

namespace Slic3r {

static Surface *to_surface(surface_handle *me) { return reinterpret_cast<Surface*>(me); }
static const Surface *to_surface(const surface_handle *me) { return reinterpret_cast<const Surface*>(me); }
static SurfaceCollection *to_surface_collection(surface_collection_handle *me) { return reinterpret_cast<SurfaceCollection*>(me); }
static const SurfaceCollection *to_surface_collection(const surface_collection_handle *me) { return reinterpret_cast<const SurfaceCollection*>(me); }

static Layer *to_layer(layer_handle *me) { return reinterpret_cast<Layer*>(me); }
static const Layer *to_layer(const layer_handle *me) { return reinterpret_cast<const Layer*>(me); }

static LayerRegion *to_layer_region(layer_region_handle *me) { return reinterpret_cast<LayerRegion*>(me); }
static const LayerRegion *to_layer_region(const layer_region_handle *me) { return reinterpret_cast<const LayerRegion*>(me); }

static LayerSliceIsland *to_layer_island(layer_island_handle *me) { return reinterpret_cast<LayerSliceIsland*>(me); }
static const LayerSliceIsland *to_layer_island(const layer_island_handle *me) { return reinterpret_cast<const LayerSliceIsland*>(me); }

static LayerRegionIsland *to_layer_region_island(layer_region_island_handle *me) { return reinterpret_cast<LayerRegionIsland*>(me); }
static const LayerRegionIsland *to_layer_region_island(const layer_region_island_handle *me) { return reinterpret_cast<const LayerRegionIsland*>(me); }

static PrintRegion *to_print_region(print_region_handle *me) { return reinterpret_cast<PrintRegion*>(me); }
static const PrintRegion *to_print_region(const print_region_handle *me) { return reinterpret_cast<const PrintRegion*>(me); }

static PrintObject *to_object(object_handle *me) { return reinterpret_cast<PrintObject*>(me); }
static const PrintObject *to_object(const object_handle *me) { return reinterpret_cast<const PrintObject*>(me); }

static Print *to_print(print_handle *me) { return reinterpret_cast<Print*>(me); }
static const Print *to_print(const print_handle *me) { return reinterpret_cast<const Print*>(me); }

static PluginPropertyContainer *to_plugin_properties(plugin_property_container_handle *me)
{
    return reinterpret_cast<PluginPropertyContainer *>(me);
}

static const PluginPropertyContainer *to_plugin_properties(const plugin_property_container_handle *me)
{
    return reinterpret_cast<const PluginPropertyContainer *>(me);
}

static plugin_property_container_handle *to_mutable_property_handle(const PluginPropertyContainer *container)
{
    return reinterpret_cast<plugin_property_container_handle *>(const_cast<PluginPropertyContainer *>(container));
}

static ExPolygon *to_expolygon(expolygon_handle *me) { return reinterpret_cast<ExPolygon*>(me); }
static const ExPolygon *to_expolygon(const expolygon_handle *me) { return reinterpret_cast<const ExPolygon*>(me); }

static ExPolygons *to_expolygons(expolygon_collection_handle *me) { return reinterpret_cast<ExPolygons*>(me); }
static const ExPolygons *to_expolygons(const expolygon_collection_handle *me) { return reinterpret_cast<const ExPolygons*>(me); }

static c_point to_c_point(const Point &point)
{
    c_point out = {};
    out.x = point.x();
    out.y = point.y();
    return out;
}

static c_bounding_box to_c_bounding_box(const BoundingBox &box)
{
    c_bounding_box out = {};
    out.min.x = box.min.x();
    out.min.y = box.min.y();
    out.max.x = box.max.x();
    out.max.y = box.max.y();
    return out;
}

static c_matrix4d to_c_matrix4d(const Transform3d &matrix)
{
    c_matrix4d out = {};
    const auto &raw = matrix.matrix();
    for (uint32_t row = 0; row < 4; ++row)
        for (uint32_t col = 0; col < 4; ++col)
            out.value[row * 4 + col] = raw(row, col);
    return out;
}

static c_flow to_c_flow(const Flow &flow)
{
    c_flow out = {};
    out.width = flow.scaled_width();
    out.spacing = flow.scaled_spacing();
    out.height = flow.scaled_height();
    out.nozzle_diameter = scale_i(flow.nozzle_diameter());
    out.is_bridge = flow.bridge() ? 1 : 0;
    out.spacing_ratio = flow.spacing_ratio();
    out.mm3_per_mm = flow.mm3_per_mm();
    return out;
}

static FlowRole to_flow_role(raw_extrusion_role role)
{
    if ((role & RAW_EXTRUSION_ROLE_SUPPORT) != 0)
        return (role & RAW_EXTRUSION_ROLE_EXTERNAL) != 0 ? frSupportMaterialInterface : frSupportMaterial;
    if ((role & RAW_EXTRUSION_ROLE_PERIMETER) != 0)
        return (role & RAW_EXTRUSION_ROLE_EXTERNAL) != 0 ? frExternalPerimeter : frPerimeter;
    if ((role & RAW_EXTRUSION_ROLE_INFILL) != 0) {
        if ((role & RAW_EXTRUSION_ROLE_EXTERNAL) != 0 && (role & RAW_EXTRUSION_ROLE_SOLID) != 0)
            return frTopSolidInfill;
        if ((role & RAW_EXTRUSION_ROLE_SOLID) != 0)
            return frSolidInfill;
    }
    return frInfill;
}

static ExtrusionRole to_extrusion_role(raw_extrusion_role role)
{
    return ExtrusionRole(static_cast<ExtrusionRoleModifier>(role));
}

static const ExPolygon *first_or_null(const ExPolygons &expolygons)
{
    return expolygons.empty() ? nullptr : &expolygons.front();
}

static BoundingBox first_extents_or_empty(const ExPolygons &expolygons)
{
    return expolygons.empty() ? BoundingBox() : get_extents(expolygons.front());
}

static ConfigBase *mutable_config(const PrintRegion &region)
{
    return const_cast<PrintRegionConfig*>(&region.config());
}

static ConfigBase *mutable_config(const PrintObject &object)
{
    return const_cast<PrintObjectConfig*>(&object.config());
}

static ConfigBase *mutable_config(const Print &print)
{
    return const_cast<PrintConfig*>(&print.config());
}

} // namespace Slic3r

extern "C" {

uint32_t plugin_property_count(const plugin_property_container_handle *me)
{
    return me == nullptr ? 0u : static_cast<uint32_t>(Slic3r::to_plugin_properties(me)->property_count());
}

plugin_property_type plugin_property_type_at(const plugin_property_container_handle *me, uint32_t idx)
{
    return me == nullptr ? PLUGIN_PROPERTY_TYPE_INVALID : Slic3r::to_plugin_properties(me)->property_type_at(idx);
}

int32_t plugin_property_has(const plugin_property_container_handle *me, plugin_property_type type)
{
    return me != nullptr && Slic3r::to_plugin_properties(me)->has_property(type);
}

uint32_t plugin_property_data_size(const plugin_property_container_handle *me, plugin_property_type type)
{
    return me == nullptr ? 0u : Slic3r::to_plugin_properties(me)->property_data_size(type);
}

const void *plugin_property_data(const plugin_property_container_handle *me, plugin_property_type type)
{
    return me == nullptr ? nullptr : Slic3r::to_plugin_properties(me)->property_data(type);
}

void *plugin_property_data_mutable(plugin_property_container_handle *me, plugin_property_type type)
{
    return me == nullptr ? nullptr : Slic3r::to_plugin_properties(me)->property_data_mutable(type);
}

void *plugin_property_get_or_add_data_mutable(orchestrator_handle *orch,
                                              plugin_property_container_handle *me,
                                              plugin_property_type type)
{
    const uint32_t byte_count = orchestrator_property_byte_count(orch, type);
    const uint32_t alignment = orchestrator_property_alignment(orch, type);
    if (byte_count == 0 || alignment == 0)
        return nullptr;
    return me == nullptr ?
               nullptr :
               Slic3r::to_plugin_properties(me)->get_or_add_property_data_mutable(type, byte_count, alignment);
}

int32_t plugin_property_remove(plugin_property_container_handle *me, plugin_property_type type)
{
    return me != nullptr && Slic3r::to_plugin_properties(me)->remove_property(type);
}

void plugin_property_clear(plugin_property_container_handle *me)
{
    if (me != nullptr)
        Slic3r::to_plugin_properties(me)->clear_properties();
}

void plugin_property_copy_all(plugin_property_container_handle *dst, const plugin_property_container_handle *src)
{
    if (dst != nullptr && src != nullptr)
        Slic3r::to_plugin_properties(dst)->copy_properties_from(*Slic3r::to_plugin_properties(src));
}

c_surface surface_c_view(const surface_handle *me)
{
    if (me == nullptr)
        return {};
    const Slic3r::Surface &surface = *Slic3r::to_surface(me);
    return c_surface{
        reinterpret_cast<const expolygon_handle *>(&surface.expolygon),
        static_cast<raw_surface_type>(surface.surface_type)
    };
}

const expolygon_handle *surface_get_expolygon(const surface_handle *me)
{
    return me == nullptr ? nullptr : reinterpret_cast<const expolygon_handle*>(&Slic3r::to_surface(me)->expolygon);
}

raw_surface_type surface_get_type(const surface_handle *me)
{
    return me == nullptr ? RAW_SURFACE_TYPE_NONE : static_cast<raw_surface_type>(Slic3r::to_surface(me)->surface_type);
}

int32_t surface_get_flag(const surface_handle *me, raw_surface_type flag)
{
    return me != nullptr && (surface_get_type(me) & flag) != 0;
}

plugin_property_container_handle *surface_get_properties(const surface_handle *me)
{
    return me == nullptr ? nullptr : Slic3r::to_mutable_property_handle(static_cast<const Slic3r::PluginPropertyContainer *>(Slic3r::to_surface(me)));
}

surface_collection_handle *storage_new_surface_collection(storage_handle *me)
{
    Slic3r::PluginStorage *storage = reinterpret_cast<Slic3r::PluginStorage *>(me);
    if (storage == nullptr)
        return nullptr;
    surface_collection_handle *out =
        reinterpret_cast<surface_collection_handle *>(&storage->surface_collections.emplace_back());
    storage->generic_storage.insert(out);
    return out;
}

void surface_collection_clear(surface_collection_handle *me)
{
    if (me != nullptr)
        Slic3r::to_surface_collection(me)->clear();
}

void surface_collection_append_expolygon_copy(surface_collection_handle *me,
                                              const expolygon_handle *area,
                                              raw_surface_type surface_type)
{
    if (me != nullptr && area != nullptr)
        Slic3r::to_surface_collection(me)->surfaces.emplace_back(
            static_cast<Slic3r::SurfaceType>(surface_type), *Slic3r::to_expolygon(area));
}

void surface_collection_append_expolygon_move(surface_collection_handle *me,
                                              expolygon_handle *area,
                                              raw_surface_type surface_type)
{
    if (me == nullptr || area == nullptr)
        return;

    Slic3r::to_surface_collection(me)->surfaces.emplace_back(
        static_cast<Slic3r::SurfaceType>(surface_type), std::move(*Slic3r::to_expolygon(area)));
    Slic3r::to_expolygon(area)->clear();
}

void surface_collection_append_expolygons_copy(surface_collection_handle *me,
                                               const expolygon_collection_handle *areas,
                                               raw_surface_type surface_type)
{
    if (me != nullptr && areas != nullptr)
        Slic3r::to_surface_collection(me)->append(*Slic3r::to_expolygons(areas),
                                                  static_cast<Slic3r::SurfaceType>(surface_type));
}

void surface_collection_append_expolygons_move(surface_collection_handle *me,
                                               expolygon_collection_handle *areas,
                                               raw_surface_type surface_type)
{
    if (me != nullptr && areas != nullptr)
        Slic3r::to_surface_collection(me)->append(std::move(*Slic3r::to_expolygons(areas)),
                                                  static_cast<Slic3r::SurfaceType>(surface_type));
}

uint32_t surface_collection_size(const surface_collection_handle *me)
{
    return me == nullptr ? 0u : uint32_t(Slic3r::to_surface_collection(me)->size());
}

const surface_handle *surface_collection_at(const surface_collection_handle *me, uint32_t idx)
{
    if (me == nullptr || idx >= Slic3r::to_surface_collection(me)->size())
        return nullptr;
    return reinterpret_cast<const surface_handle *>(&Slic3r::to_surface_collection(me)->at(idx));
}

surface_handle *surface_collection_at_mutable(surface_collection_handle *me, uint32_t idx)
{
    if (me == nullptr || idx >= Slic3r::to_surface_collection(me)->size())
        return nullptr;
    return reinterpret_cast<surface_handle *>(&Slic3r::to_surface_collection(me)->at(idx));
}

coord_t layer_get_height(const layer_handle *me)
{
    return me == nullptr ? 0 : Slic3r::to_layer(me)->scaled_height();
}

coord_t layer_get_print_z(const layer_handle *me)
{
    return me == nullptr ? 0 : Slic3r::to_layer(me)->scaled_print_z();
}

coord_t layer_get_slice_z(const layer_handle *me)
{
    return me == nullptr ? 0 : Slic3r::to_layer(me)->scaled_print_z() - Slic3r::to_layer(me)->scaled_height() / 2;
}

coord_t layer_get_support_id(const layer_handle *me)
{
    if (me == nullptr)
        return -1;
    const Slic3r::SupportLayer *support = dynamic_cast<const Slic3r::SupportLayer*>(Slic3r::to_layer(me));
    return support == nullptr ? -1 : static_cast<coord_t>(support->interface_id());
}

const expolygon_collection_handle *layer_get_slices(const layer_handle *me) {
    return me == nullptr ? nullptr : reinterpret_cast<const expolygon_collection_handle*>(&Slic3r::to_layer(me)->lslices());
}

uint32_t layer_count_curled_line(const layer_handle *me)
{
    return me == nullptr ? 0u : uint32_t(Slic3r::to_layer(me)->curled_lines.size());
}

c_curled_line layer_get_curled_line(const layer_handle *me, uint32_t idx)
{
    c_curled_line out = {};
    if (me == nullptr || idx >= Slic3r::to_layer(me)->curled_lines.size())
        return out;

    const Slic3r::CurledLine &line = Slic3r::to_layer(me)->curled_lines[idx];
    out.a = Slic3r::to_c_point(line.a);
    out.b = Slic3r::to_c_point(line.b);
    out.curled_height = line.curled_height;
    return out;
}

layer_handle *layer_get_upper_layer_mutable(layer_handle *me)
{
    return me == nullptr ? nullptr : reinterpret_cast<layer_handle*>(Slic3r::to_layer(me)->upper_layer);
}

const layer_handle *layer_get_upper_layer(const layer_handle *me)
{
    return me == nullptr ? nullptr : reinterpret_cast<const layer_handle*>(Slic3r::to_layer(me)->upper_layer);
}

layer_handle *layer_get_lower_layer_mutable(layer_handle *me)
{
    return me == nullptr ? nullptr : reinterpret_cast<layer_handle*>(Slic3r::to_layer(me)->lower_layer);
}

const layer_handle *layer_get_lower_layer(const layer_handle *me)
{
    return me == nullptr ? nullptr : reinterpret_cast<const layer_handle*>(Slic3r::to_layer(me)->lower_layer);
}

plugin_property_container_handle *layer_get_properties(const layer_handle *me)
{
    return me == nullptr ? nullptr : Slic3r::to_mutable_property_handle(static_cast<const Slic3r::PluginPropertyContainer *>(Slic3r::to_layer(me)));
}

uint32_t layer_count_region(const layer_handle *me)
{
    return me == nullptr ? 0 : Slic3r::to_layer(me)->region_count();
}

layer_region_handle *layer_get_region_mutable(layer_handle *me, uint32_t idx)
{
    if (me == nullptr || idx >= Slic3r::to_layer(me)->region_count())
        return nullptr;
    return reinterpret_cast<layer_region_handle*>(&Slic3r::to_layer(me)->region(idx));
}

const layer_region_handle *layer_get_region(const layer_handle *me, uint32_t idx)
{
    if (me == nullptr || idx >= Slic3r::to_layer(me)->region_count())
        return nullptr;
    return reinterpret_cast<const layer_region_handle*>(&Slic3r::to_layer(me)->region(idx));
}

uint32_t layer_count_island(const layer_handle *me)
{
    return me == nullptr ? 0 : Slic3r::to_layer(me)->islands().size();
}

layer_island_handle *layer_get_island_mutable(layer_handle *me, uint32_t idx)
{
    if (me == nullptr || idx >= Slic3r::to_layer(me)->islands().size())
        return nullptr;
    return reinterpret_cast<layer_island_handle*>(&Slic3r::to_layer(me)->island(idx));
}

const layer_island_handle *layer_get_island(const layer_handle *me, uint32_t idx)
{
    if (me == nullptr || idx >= Slic3r::to_layer(me)->islands().size())
        return nullptr;
    return reinterpret_cast<const layer_island_handle*>(&Slic3r::to_layer(me)->island(idx));
}

plugin_property_container_handle *layer_region_get_properties(const layer_region_handle *me)
{
    return me == nullptr ? nullptr : Slic3r::to_mutable_property_handle(static_cast<const Slic3r::PluginPropertyContainer *>(Slic3r::to_layer_region(me)));
}

c_flow layer_region_get_flow(const layer_region_handle *me, raw_extrusion_role flow_role)
{
    c_flow out = {};
    if (me == nullptr)
        return out;
    return Slic3r::to_c_flow(Slic3r::to_layer_region(me)->flow(Slic3r::to_flow_role(flow_role)));
}

c_flow layer_region_get_bridging_flow(const layer_region_handle *me, raw_extrusion_role flow_role)
{
    c_flow out = {};
    if (me == nullptr)
        return out;
    return Slic3r::to_c_flow(Slic3r::to_layer_region(me)->bridging_flow(Slic3r::to_flow_role(flow_role)));
}

const expolygon_collection_handle *layer_region_get_slices(const layer_region_handle *me)
{
    return me == nullptr ? nullptr : reinterpret_cast<const expolygon_collection_handle*>(&Slic3r::to_layer_region(me)->get_raw_slices());
}

c_bounding_box layer_region_get_bounding_box(const layer_region_handle *me)
{
    return me == nullptr ? c_bounding_box{} : Slic3r::to_c_bounding_box(Slic3r::get_extents(*Slic3r::to_layer_region(me)));
}

const layer_handle *layer_region_get_layer(const layer_region_handle *me)
{
    return me == nullptr ? nullptr : reinterpret_cast<const layer_handle*>(Slic3r::to_layer_region(me)->layer());
}

const print_region_handle *layer_region_get_print_region(const layer_region_handle *me)
{
    return me == nullptr ? nullptr : reinterpret_cast<const print_region_handle*>(&Slic3r::to_layer_region(me)->region());
}

expolygon_handle *layer_island_get_slice_mutable(layer_island_handle *me)
{
    return me == nullptr ? nullptr :
                           reinterpret_cast<expolygon_handle *>(
                               &Slic3r::ApiInternal::LayerIslandAccess::slice_mutable(*Slic3r::to_layer_island(me)));
}

const expolygon_handle *layer_island_get_slice(const layer_island_handle *me)
{
    return me == nullptr ? nullptr : reinterpret_cast<const expolygon_handle*>(&Slic3r::to_layer_island(me)->get_slice());
}

c_bounding_box layer_island_get_bounding_box(const layer_island_handle *me)
{
    return me == nullptr ? c_bounding_box{} : Slic3r::to_c_bounding_box(Slic3r::to_layer_island(me)->get_bounding_box());
}

const expolygon_handle *layer_island_get_infill_slice(const layer_island_handle *me)
{
    if (me == nullptr)
        return nullptr;
    const Slic3r::ExPolygon *expoly = Slic3r::first_or_null(Slic3r::to_layer_island(me)->infill_areas());
    return reinterpret_cast<const expolygon_handle*>(expoly);
}

const expolygon_collection_handle *layer_island_get_infill_areas(const layer_island_handle *me)
{
    return me == nullptr ? nullptr :
                           reinterpret_cast<const expolygon_collection_handle *>(
                               &Slic3r::to_layer_island(me)->infill_areas());
}

c_bounding_box layer_island_get_infill_bounding_box(const layer_island_handle *me)
{
    return me == nullptr ? c_bounding_box{} : Slic3r::to_c_bounding_box(Slic3r::first_extents_or_empty(Slic3r::to_layer_island(me)->infill_areas()));
}

const expolygon_handle *layer_island_get_infill_no_overlap_slice(const layer_island_handle *me)
{
    if (me == nullptr)
        return nullptr;
    const Slic3r::ExPolygon *expoly = Slic3r::first_or_null(Slic3r::to_layer_island(me)->infill_free_areas());
    return reinterpret_cast<const expolygon_handle*>(expoly);
}

const expolygon_collection_handle *layer_island_get_infill_no_overlap_areas(const layer_island_handle *me)
{
    return me == nullptr ? nullptr :
                           reinterpret_cast<const expolygon_collection_handle *>(
                               &Slic3r::to_layer_island(me)->infill_free_areas());
}

plugin_property_container_handle *layer_island_get_properties(const layer_island_handle *me)
{
    return me == nullptr ? nullptr : Slic3r::to_mutable_property_handle(static_cast<const Slic3r::PluginPropertyContainer *>(Slic3r::to_layer_island(me)));
}

uint32_t layer_island_count_region(const layer_island_handle *me)
{
    return me == nullptr ? 0 : Slic3r::to_layer_island(me)->regions().size();
}

layer_region_handle *layer_island_get_region_mutable(layer_island_handle *me, uint32_t idx)
{
    if (me == nullptr || idx >= Slic3r::to_layer_island(me)->regions().size())
        return nullptr;
    auto it = Slic3r::to_layer_island(me)->regions().begin();
    std::advance(it, static_cast<std::ptrdiff_t>(idx));
    return reinterpret_cast<layer_region_handle*>(const_cast<Slic3r::LayerRegion*>(*it));
}

const layer_region_handle *layer_island_get_region(const layer_island_handle *me, uint32_t idx)
{
    if (me == nullptr || idx >= Slic3r::to_layer_island(me)->regions().size())
        return nullptr;
    auto it = Slic3r::to_layer_island(me)->regions().begin();
    std::advance(it, static_cast<std::ptrdiff_t>(idx));
    return reinterpret_cast<const layer_region_handle*>(*it);
}

uint32_t layer_island_count_region_island(const layer_island_handle *me)
{
    return me == nullptr ? 0 : Slic3r::to_layer_island(me)->regions_islands().size();
}

layer_region_island_handle *layer_island_get_region_island_mutable(layer_island_handle *me, uint32_t idx)
{
    if (me == nullptr || idx >= Slic3r::to_layer_island(me)->regions_islands().size())
        return nullptr;
    return reinterpret_cast<layer_region_island_handle*>(&Slic3r::to_layer_island(me)->regions_island(idx));
}

const layer_region_island_handle *layer_island_get_region_island(const layer_island_handle *me, uint32_t idx)
{
    if (me == nullptr || idx >= Slic3r::to_layer_island(me)->regions_islands().size())
        return nullptr;
    return reinterpret_cast<const layer_region_island_handle*>(&Slic3r::to_layer_island(me)->regions_island(idx));
}

const layer_handle *layer_island_get_layer(const layer_island_handle *me)
{
    return me == nullptr ? nullptr : reinterpret_cast<const layer_handle*>(Slic3r::to_layer_island(me)->layer());
}

uint32_t layer_island_count_lower_island(const layer_island_handle *me)
{
    return me == nullptr ? 0 : static_cast<uint32_t>(Slic3r::to_layer_island(me)->overlaps_below.size());
}

const layer_island_handle *layer_island_get_lower_island(const layer_island_handle *me, uint32_t idx)
{
    if (me == nullptr || idx >= Slic3r::to_layer_island(me)->overlaps_below.size())
        return nullptr;
    return reinterpret_cast<const layer_island_handle *>(Slic3r::to_layer_island(me)->overlaps_below[idx].to);
}

uint32_t layer_island_count_upper_island(const layer_island_handle *me)
{
    return me == nullptr ? 0 : static_cast<uint32_t>(Slic3r::to_layer_island(me)->overlaps_above.size());
}

const layer_island_handle *layer_island_get_upper_island(const layer_island_handle *me, uint32_t idx)
{
    if (me == nullptr || idx >= Slic3r::to_layer_island(me)->overlaps_above.size())
        return nullptr;
    return reinterpret_cast<const layer_island_handle *>(Slic3r::to_layer_island(me)->overlaps_above[idx].to);
}

int32_t layer_region_island_extruder_id(const layer_region_island_handle *me)
{
    return me == nullptr ? -1 : static_cast<int32_t>(Slic3r::to_layer_region_island(me)->extruder_id());
}

int32_t layer_region_island_has_extrusions(const layer_region_island_handle *me)
{
    return me != nullptr && Slic3r::to_layer_region_island(me)->has_extrusions();
}

int32_t layer_region_island_has_extrusion(const layer_region_island_handle *me, raw_extrusion_role role)
{
    return me != nullptr && Slic3r::to_layer_region_island(me)->has_extrusion(Slic3r::to_extrusion_role(role));
}

extrusion_entity_handle *layer_region_island_get_mutable_extrusion(layer_region_island_handle *me, raw_extrusion_role role)
{
    if (me == nullptr)
        return nullptr;
    Slic3r::ExtrusionEntityCollection &collection = Slic3r::to_layer_region_island(me)->mutable_extrusion(Slic3r::to_extrusion_role(role));
    return reinterpret_cast<extrusion_entity_handle*>(&collection);
}

const extrusion_entity_handle *layer_region_island_get_extrusion(const layer_region_island_handle *me, raw_extrusion_role role)
{
    if (me == nullptr || !Slic3r::to_layer_region_island(me)->has_extrusion(Slic3r::to_extrusion_role(role)))
        return nullptr;
    return reinterpret_cast<const extrusion_entity_handle*>(&Slic3r::to_layer_region_island(me)->extrusion(Slic3r::to_extrusion_role(role)));
}

const surface_collection_handle *layer_region_island_get_fill_surfaces(const layer_region_island_handle *me)
{
    return me == nullptr ?
               nullptr :
               reinterpret_cast<const surface_collection_handle *>(&Slic3r::to_layer_region_island(me)->fill_surfaces());
}

uint32_t layer_region_island_count_fill_surface(const layer_region_island_handle *me)
{
    return surface_collection_size(layer_region_island_get_fill_surfaces(me));
}

const surface_handle *layer_region_island_get_fill_surface(const layer_region_island_handle *me, uint32_t idx)
{
    return surface_collection_at(layer_region_island_get_fill_surfaces(me), idx);
}

plugin_property_container_handle *layer_region_island_get_properties(const layer_region_island_handle *me)
{
    return me == nullptr ? nullptr : Slic3r::to_mutable_property_handle(static_cast<const Slic3r::PluginPropertyContainer *>(Slic3r::to_layer_region_island(me)));
}

uint32_t layer_region_island_count_region(const layer_region_island_handle *me)
{
    return me == nullptr ? 0 : uint32_t(Slic3r::to_layer_region_island(me)->regions().size());
}

const layer_region_handle *layer_region_island_get_region(const layer_region_island_handle *me, uint32_t idx)
{
    if (me == nullptr || idx >= layer_region_island_count_region(me))
        return nullptr;

    Slic3r::LayerRegionSetCPtrs::const_iterator it = Slic3r::to_layer_region_island(me)->regions().begin();
    std::advance(it, idx);
    return reinterpret_cast<const layer_region_handle *>(*it);
}

config_handle *print_region_get_config_mutable(print_region_handle *me)
{
    return me == nullptr ? nullptr : Slic3r::ApiHost::to_config_handle(Slic3r::mutable_config(*Slic3r::to_print_region(me)));
}

const config_handle *print_region_get_config(const print_region_handle *me)
{
    return me == nullptr ? nullptr : Slic3r::ApiHost::to_config_handle(&Slic3r::to_print_region(me)->config());
}

config_handle *object_get_config_mutable(object_handle *me)
{
    return me == nullptr ? nullptr : Slic3r::ApiHost::to_config_handle(Slic3r::mutable_config(*Slic3r::to_object(me)));
}

const config_handle *object_get_config(const object_handle *me)
{
    return me == nullptr ? nullptr : Slic3r::ApiHost::to_config_handle(&Slic3r::to_object(me)->config());
}

plugin_property_container_handle *object_get_properties(const object_handle *me)
{
    return me == nullptr ? nullptr : Slic3r::to_mutable_property_handle(static_cast<const Slic3r::PluginPropertyContainer *>(Slic3r::to_object(me)));
}

coord_t object_get_max_z(const object_handle *me)
{
    return me == nullptr || Slic3r::to_object(me)->model_object() == nullptr ?
               coord_t(0) :
               scale_i(Slic3r::to_object(me)->model_object()->max_z());
}

c_matrix4d object_get_transform(const object_handle *me)
{
    return me == nullptr ? c_matrix4d{} : Slic3r::to_c_matrix4d(Slic3r::to_object(me)->trafo());
}

c_point object_get_center_offset(const object_handle *me)
{
    return me == nullptr ? c_point{} : Slic3r::to_c_point(Slic3r::to_object(me)->center_offset());
}

uint32_t object_count_layer(const object_handle *me)
{
    return me == nullptr ? 0 : Slic3r::to_object(me)->layer_count();
}

layer_handle *object_get_layer_mutable(object_handle *me, uint32_t idx)
{
    if (me == nullptr || idx >= Slic3r::to_object(me)->layer_count())
        return nullptr;
    return reinterpret_cast<layer_handle*>(&Slic3r::to_object(me)->layer(static_cast<size_t>(idx)));
}

const layer_handle *object_get_layer(const object_handle *me, uint32_t idx)
{
    if (me == nullptr || idx >= Slic3r::to_object(me)->layer_count())
        return nullptr;
    return reinterpret_cast<const layer_handle*>(&Slic3r::to_object(me)->layer(static_cast<size_t>(idx)));
}

uint32_t object_count_region(const object_handle *me)
{
    return me == nullptr ? 0 : Slic3r::to_object(me)->num_printing_regions();
}

print_region_handle *object_get_print_region_mutable(object_handle *me, uint32_t idx)
{
    if (me == nullptr || idx >= Slic3r::to_object(me)->num_printing_regions())
        return nullptr;
    return reinterpret_cast<print_region_handle*>(const_cast<Slic3r::PrintRegion*>(&Slic3r::to_object(me)->printing_region(idx)));
}

const print_region_handle *object_get_print_region(const object_handle *me, uint32_t idx)
{
    if (me == nullptr || idx >= Slic3r::to_object(me)->num_printing_regions())
        return nullptr;
    return reinterpret_cast<const print_region_handle*>(&Slic3r::to_object(me)->printing_region(idx));
}

config_handle *print_get_config_mutable(print_handle *me)
{
    return me == nullptr ? nullptr : Slic3r::ApiHost::to_config_handle(Slic3r::mutable_config(*Slic3r::to_print(me)));
}

const config_handle *print_get_config(const print_handle *me)
{
    return me == nullptr ? nullptr : Slic3r::ApiHost::to_config_handle(&Slic3r::to_print(me)->config());
}

uint32_t print_count_object(const print_handle *me)
{
    return me == nullptr ? 0 : Slic3r::to_print(me)->objects().size();
}

object_handle *print_get_object_mutable(print_handle *me, uint32_t idx)
{
    if (me == nullptr || idx >= Slic3r::to_print(me)->objects().size())
        return nullptr;
    return reinterpret_cast<object_handle*>(&Slic3r::to_print(me)->object(idx));
}

const object_handle *print_get_object(const print_handle *me, uint32_t idx)
{
    if (me == nullptr || idx >= Slic3r::to_print(me)->objects().size())
        return nullptr;
    return reinterpret_cast<const object_handle*>(&Slic3r::to_print(me)->object(idx));
}

const_strings_t config_keys(const config_handle *me)
{
    const_strings_t out = {};
    if (me == nullptr)
        return out;

    static thread_local std::vector<std::string> key_storage;
    static thread_local std::vector<const char*> key_ptrs;

    key_storage = Slic3r::ApiHost::to_config(me)->keys();
    key_ptrs.clear();
    key_ptrs.reserve(key_storage.size());
    for (const std::string &key : key_storage)
        key_ptrs.push_back(key.c_str());

    out.items = key_ptrs.empty() ? nullptr : key_ptrs.data();
    out.size = key_ptrs.size();
    return out;
}

const config_option_handle *config_get(const config_handle *me, const char *key)
{
    if (me == nullptr || key == nullptr)
        return nullptr;
    const Slic3r::ConfigBase * config = Slic3r::ApiHost::to_config(me);
    const Slic3r::ConfigOption * opt = config->option(key);
    return reinterpret_cast<const config_option_handle*>(opt);
}

config_option_handle *config_get_mutable(config_handle *me, const char *key)
{
    if (me == nullptr || key == nullptr)
        return nullptr;
    return reinterpret_cast<config_option_handle*>(Slic3r::ApiHost::to_config(me)->optptr(key, false));
}

} // extern "C"
