
///|/ Copyright (c) SuperSlicer 2026 Durand Rémi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/

#include "StepSkirtBrim.hpp"

#include <cassert>
#include <memory>
#include <vector>

#include "libslic3r/Api/internal/LayerAccess.hpp"
#include "libslic3r/Api/internal/LayerRegionAccess.hpp"
#include "libslic3r/Api/host/Orchestrator.hpp"
#include "libslic3r/Api/host/Plugin.hpp"
#include "libslic3r/Api/internal/PrintAccess.hpp"
#include "libslic3r/Api/internal/PrintObjectAccess.hpp"
#include "libslic3r/Api/plugin/c/slic3r_data_tree.h"
#include "libslic3r/Api/plugin/c/steps/slic3r_step_skirt_brim.h"
#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/Exception.hpp"
#include "libslic3r/ExtrusionEntity.hpp"
#include "libslic3r/Layer.hpp"
#include "libslic3r/LayerRegion.hpp"
#include "libslic3r/PluginProperty.hpp"
#include "libslic3r/Polygon.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintObject.hpp"
#include "libslic3r/Steps/StepPipeline.hpp"

namespace Slic3r::Steps::StepSkirtBrim {
namespace {

Print *to_print(print_handle *handle)
{
    return reinterpret_cast<Print *>(handle);
}

PrintObject *to_object(object_handle *handle)
{
    return reinterpret_cast<PrintObject *>(handle);
}

ExtrusionEntity *to_extrusion(extrusion_entity_handle *handle)
{
    return reinterpret_cast<ExtrusionEntity *>(handle);
}

const extrusion_entity_handle *to_handle(const ExtrusionEntity *entity)
{
    return reinterpret_cast<const extrusion_entity_handle *>(entity);
}

Polygons *to_polygons(polygon_collection_handle *handle)
{
    return reinterpret_cast<Polygons *>(handle);
}

ExPolygons object_brim_subject_from_extrusion(const ExtrusionEntity &extrusion)
{
    /*
    A layer slice is area, while brim extrusions are centerline paths. Inflate
    each leaf by its own stored width / 2 through the extrusion helper, then
    union the result so the auxiliary layer owns a clean printable subject.
    */
    Polygons coverage = extrusion.polygons_covered_by_width(float(SCALED_EPSILON));
    return coverage.empty() ? ExPolygons{} : union_ex(coverage);
}

Layer *create_object_brim_auxiliary_layer(PrintObject &object)
{
    if (object.layer_count() == 0)
        return nullptr;

    /*
    Object brim lives on the first printable plane. Use the first object layer
    as the source of height and Z so future ordering sees the brim at the same
    vertical position as the old object-brim mirror.
    */
    const Layer &first_layer = object.layer(0);
    layer_handle *created = object_add_auxiliary_layer(
        reinterpret_cast<const object_handle *>(&object),
        first_layer.scaled_height(),
        first_layer.scaled_print_z(),
        scale_to_layer_coord(first_layer.slice_z));
    if (created == nullptr)
        return nullptr;

    Layer *layer = reinterpret_cast<Layer *>(created);
    LayerBrimProperty &property = layer->get_or_add_property<LayerBrimProperty>();
    property.reserved = 0;
    return layer;
}

void remove_auxiliary_layer(PrintObject &object, Layer &layer)
{
    (void) object_remove_auxiliary_layer(
        reinterpret_cast<const object_handle *>(&object),
        reinterpret_cast<layer_handle *>(&layer));
}

bool publish_object_brim_to_auxiliary_layer(PrintObject &object, ExtrusionEntity &extrusion)
{
    if (extrusion.empty())
        return false;

    ExPolygons subject = object_brim_subject_from_extrusion(extrusion);
    if (subject.empty())
        return false;

    /*
    The layer is built for this publication only. Layer islands become locked
    after add_regions_to_islands(), so reusing an existing brim layer would need
    a broader rebuild API and could accidentally drop already-published paths.
    */
    Layer *layer = create_object_brim_auxiliary_layer(object);
    if (layer == nullptr)
        return false;

    if (layer->region_count() == 0) {
        remove_auxiliary_layer(object, *layer);
        return false;
    }

    /*
    Region zero is the fallback region for auxiliary geometry. Brim currently
    has no region-specific split, so every other raw-slice bucket stays empty
    and the fallback region owns the full subject.
    */
    for (LayerRegion &region : layer->regions())
        ApiInternal::LayerRegionAccess::slices_mutable(region).clear();
    ApiInternal::LayerRegionAccess::slices_mutable(layer->region(0)) = std::move(subject);

    /*
    Rebuild the derived layer shape exactly as post-slicing does: raw
    LayerRegion slices produce Layer islands, then each island receives the
    LayerRegionIsland objects needed to hold extrusion roots.
    */
    ApiInternal::LayerAccess::recompute_slices_from_layer_regions(*layer);
    if (layer->islands().empty()) {
        remove_auxiliary_layer(object, *layer);
        return false;
    }
    layer->add_regions_to_islands();
    if (layer->islands().empty() || layer->island(0).regions().empty()) {
        remove_auxiliary_layer(object, *layer);
        return false;
    }

    /*
    Store the original extrusion root in the general perimeter bucket. The
    paths keep their ExtrusionAttributes role Skirt, which is how the G-code
    and preview layers identify skirt/brim material today.
    */
    LayerRegionIsland &region_island = layer->island(0).get_or_add_region_island(layer->island(0).regions());
    region_island.mutable_extrusion(LayerRegionIsland::PERIMETERS).append(std::move(extrusion));
    return true;
}

int32_t clear_brim_callback(print_handle *print_handle_value)
{
    Print *print = to_print(print_handle_value);
    if (print == nullptr)
        return 0;

    ApiInternal::PrintAccess::clear_brim(*print);
    return 1;
}

int32_t clear_object_brim_callback(object_handle *object_handle_value)
{
    PrintObject *object = to_object(object_handle_value);
    if (object == nullptr)
        return 0;

    ApiInternal::PrintObjectAccess::mutable_brim(*object).clear();
    ApiInternal::PrintObjectAccess::clear_brim_auxiliary_layers(*object);
    return 1;
}

int32_t clear_skirt_callback(print_handle *print_handle_value)
{
    Print *print = to_print(print_handle_value);
    if (print == nullptr)
        return 0;

    ApiInternal::PrintAccess::clear_skirt(*print);
    return 1;
}

int32_t clear_object_skirt_callback(object_handle *object_handle_value)
{
    PrintObject *object = to_object(object_handle_value);
    if (object == nullptr)
        return 0;

    ApiInternal::PrintObjectAccess::mutable_skirt(*object).clear();
    ApiInternal::PrintObjectAccess::mutable_skirt_first_layer(*object).reset();
    return 1;
}

int32_t append_brim_move_callback(print_handle *print_handle_value,
                                  extrusion_entity_handle *extrusion_handle_value)
{
    Print *print = to_print(print_handle_value);
    ExtrusionEntity *extrusion = to_extrusion(extrusion_handle_value);
    if (print == nullptr || extrusion == nullptr)
        return 0;

    return ApiInternal::PrintAccess::append_brim_move(*print, *extrusion) ? 1 : 0;
}

int32_t append_object_brim_move_callback(object_handle *object_handle_value,
                                         extrusion_entity_handle *extrusion_handle_value)
{
    PrintObject *object = to_object(object_handle_value);
    ExtrusionEntity *extrusion = to_extrusion(extrusion_handle_value);
    if (object == nullptr || extrusion == nullptr)
        return 0;

    /*
    Keep m_brim as a deprecated mirror while publishing the structured output
    into an auxiliary layer. The mirror is cloned before the move so legacy
    G-code, preview and skirt code continue to see the same object brim.
    */
    ExtrusionEntityUPtr legacy_mirror(extrusion->clone());
    if (publish_object_brim_to_auxiliary_layer(*object, *extrusion))
        return ApiInternal::PrintObjectAccess::append_brim_move(*object, *legacy_mirror) ? 1 : 0;

    return ApiInternal::PrintObjectAccess::append_brim_move(*object, *extrusion) ? 1 : 0;
}

int32_t append_skirt_move_callback(print_handle *print_handle_value,
                                   extrusion_entity_handle *extrusion_handle_value)
{
    Print *print = to_print(print_handle_value);
    ExtrusionEntity *extrusion = to_extrusion(extrusion_handle_value);
    if (print == nullptr || extrusion == nullptr)
        return 0;

    return ApiInternal::PrintAccess::append_skirt_move(*print, *extrusion) ? 1 : 0;
}

int32_t append_object_skirt_move_callback(object_handle *object_handle_value,
                                          extrusion_entity_handle *extrusion_handle_value)
{
    PrintObject *object = to_object(object_handle_value);
    ExtrusionEntity *extrusion = to_extrusion(extrusion_handle_value);
    if (object == nullptr || extrusion == nullptr)
        return 0;

    return ApiInternal::PrintObjectAccess::append_skirt_move(*object, *extrusion) ? 1 : 0;
}

int32_t append_skirt_first_layer_move_callback(print_handle *print_handle_value,
                                               extrusion_entity_handle *extrusion_handle_value)
{
    Print *print = to_print(print_handle_value);
    ExtrusionEntity *extrusion = to_extrusion(extrusion_handle_value);
    if (print == nullptr || extrusion == nullptr)
        return 0;

    return ApiInternal::PrintAccess::append_skirt_first_layer_move(*print, *extrusion) ? 1 : 0;
}

int32_t append_object_skirt_first_layer_move_callback(object_handle *object_handle_value,
                                                      extrusion_entity_handle *extrusion_handle_value)
{
    PrintObject *object = to_object(object_handle_value);
    ExtrusionEntity *extrusion = to_extrusion(extrusion_handle_value);
    if (object == nullptr || extrusion == nullptr)
        return 0;

    return ApiInternal::PrintObjectAccess::append_skirt_first_layer_move(*object, *extrusion) ? 1 : 0;
}

int32_t append_skirt_convex_hull_move_callback(print_handle *print_handle_value,
                                               polygon_collection_handle *polygons_handle_value)
{
    Print *print = to_print(print_handle_value);
    Polygons *polygons = to_polygons(polygons_handle_value);
    if (print == nullptr || polygons == nullptr)
        return 0;

    return ApiInternal::PrintAccess::append_skirt_convex_hull_move(*print, *polygons) ? 1 : 0;
}

const extrusion_entity_handle *get_brim_callback(const print_handle *print_handle_value)
{
    const Print *print = reinterpret_cast<const Print *>(print_handle_value);
    return print == nullptr ? nullptr : reinterpret_cast<const extrusion_entity_handle *>(&print->brim());
}

const extrusion_entity_handle *get_object_brim_callback(const object_handle *object_handle_value)
{
    const PrintObject *object = reinterpret_cast<const PrintObject *>(object_handle_value);
    return object == nullptr ? nullptr : reinterpret_cast<const extrusion_entity_handle *>(&object->brim());
}

const extrusion_entity_handle *get_skirt_callback(const print_handle *print_handle_value)
{
    const Print *print = reinterpret_cast<const Print *>(print_handle_value);
    return print == nullptr ? nullptr : reinterpret_cast<const extrusion_entity_handle *>(&print->skirt());
}

const extrusion_entity_handle *get_object_skirt_callback(const object_handle *object_handle_value)
{
    const PrintObject *object = reinterpret_cast<const PrintObject *>(object_handle_value);
    return object == nullptr ? nullptr : reinterpret_cast<const extrusion_entity_handle *>(&object->skirt());
}

const extrusion_entity_handle *get_skirt_first_layer_callback(const print_handle *print_handle_value)
{
    const Print *print = reinterpret_cast<const Print *>(print_handle_value);
    return print == nullptr ? nullptr : to_handle(ApiInternal::PrintAccess::skirt_first_layer(*print));
}

const extrusion_entity_handle *get_object_skirt_first_layer_callback(const object_handle *object_handle_value)
{
    const PrintObject *object = reinterpret_cast<const PrintObject *>(object_handle_value);
    return object == nullptr ? nullptr : to_handle(ApiInternal::PrintObjectAccess::skirt_first_layer(*object));
}

run_ctx_skirt_brim payload_for_print(Print &print)
{
    run_ctx_skirt_brim payload = {};
    payload.print = reinterpret_cast<print_handle *>(&print);
    payload.clear_brim = &clear_brim_callback;
    payload.clear_object_brim = &clear_object_brim_callback;
    payload.clear_skirt = &clear_skirt_callback;
    payload.clear_object_skirt = &clear_object_skirt_callback;
    payload.append_brim_move = &append_brim_move_callback;
    payload.append_object_brim_move = &append_object_brim_move_callback;
    payload.append_skirt_move = &append_skirt_move_callback;
    payload.append_object_skirt_move = &append_object_skirt_move_callback;
    payload.append_skirt_first_layer_move = &append_skirt_first_layer_move_callback;
    payload.append_object_skirt_first_layer_move = &append_object_skirt_first_layer_move_callback;
    payload.append_skirt_convex_hull_move = &append_skirt_convex_hull_move_callback;
    payload.get_brim = &get_brim_callback;
    payload.get_object_brim = &get_object_brim_callback;
    payload.get_skirt = &get_skirt_callback;
    payload.get_object_skirt = &get_object_skirt_callback;
    payload.get_skirt_first_layer = &get_skirt_first_layer_callback;
    payload.get_object_skirt_first_layer = &get_object_skirt_first_layer_callback;
    return payload;
}

} // namespace

void clean_and_prepare(Print &print)
{
    /*
    STEP_SKIRT_BRIM is append-oriented: each active plugin publishes the brim
    pieces it owns. Clear previous brim output once before the chain starts so
    disabled plugins cannot leave stale adhesion geometry behind.
    */
    ApiInternal::PrintAccess::clear_brim(print);
    ApiInternal::PrintAccess::clear_skirt(print);
}

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
    std::vector<Plugin *> plugins =
        selected_or_active_plugins_for_step(orchestrator, STEP_SKIRT_BRIM, &print.full_print_config());
    run_ctx_skirt_brim payload = payload_for_print(print);

    for (Plugin *plugin : plugins) {
        if (plugin == nullptr)
            continue;

        /*
        Each skirt/brim plugin runs once for the whole print. A plugin that
        needs per-object behavior can iterate print.objects() itself; this
        keeps global-vs-object brim decisions inside the generator.
        */
        plugin_host_context host_context = orchestrator.prepare_plugin_host_context(STEP_SKIRT_BRIM, plugin, &print);
        host_context.object_count = print.objects().size();
        plugin_run_context run_context =
            orchestrator.prepare_plugin_run_context(STEP_SKIRT_BRIM, plugin, &host_context);
        run_context.data = &payload;

        plugin->setup(run_context, 1);
        plugin->setup_run(run_context);
        plugin->run(run_context);

        if (run_context.is_cancelled != nullptr && run_context.is_cancelled(run_context.host_context))
            throw RuntimeError("Skirt/brim plugin failed.");
    }

    ApiInternal::PrintAccess::normalize_skirt_brim_direction(print);
    ApiInternal::PrintAccess::rebuild_first_layer_convex_hull_after_skirt_brim(print);
}

} // namespace Slic3r::Steps::StepSkirtBrim
