
///|/ Copyright (c) SuperSlicer 2026 Durand Rémi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/

#include "StepSkirtBrim.hpp"

#include <cassert>
#include <vector>

#include "libslic3r/Api/host/Orchestrator.hpp"
#include "libslic3r/Api/host/Plugin.hpp"
#include "libslic3r/Api/internal/PrintAccess.hpp"
#include "libslic3r/Api/internal/PrintObjectAccess.hpp"
#include "libslic3r/Api/plugin/c/steps/slic3r_step_skirt_brim.h"
#include "libslic3r/Exception.hpp"
#include "libslic3r/ExtrusionEntity.hpp"
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

    return ApiInternal::PrintObjectAccess::append_brim_move(*object, *extrusion) ? 1 : 0;
}

run_ctx_skirt_brim payload_for_print(Print &print)
{
    run_ctx_skirt_brim payload = {};
    payload.print = reinterpret_cast<print_handle *>(&print);
    payload.clear_brim = &clear_brim_callback;
    payload.clear_object_brim = &clear_object_brim_callback;
    payload.append_brim_move = &append_brim_move_callback;
    payload.append_object_brim_move = &append_object_brim_move_callback;
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

    ApiInternal::PrintAccess::normalize_brim_direction(print);
    ApiInternal::PrintAccess::rebuild_first_layer_convex_hull_after_skirt_brim(print);
}

} // namespace Slic3r::Steps::StepSkirtBrim
