///|/ Copyright (c) SuperSlicer 2026 Durand Rémi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/

#include "SupportDemandPainting.hpp"

#include <cstdint>
#include <utility>
#include <vector>

#include "libslic3r/Api/plugin/c/slic3r_orchestrator.h"
#include "libslic3r/Api/plugin/c/steps/slic3r_step_support_demand.h"
#include "libslic3r/Api/plugin/cpp/Views.hpp"

/*
SupportDemandPainting
=====================

This plugin applies the support-enforcer and support-blocker areas painted on
the object to the support-demand polygons. It runs at STEP_SUPPORT_DEMAND,
before support geometry is generated, and operates on the demand left by
other support-demand plugins.

The normal execution flow is:

    setup_run_impl()
    `-- reserve progress for every layer island when support painting exists

    run_impl()
    |-- project the painted support facets into per-layer polygons
    `-- for every layer island:
        |-- intersect enforcer painting with the island and union it with demand
        `-- enlarge blocker painting slightly and subtract it from demand

Enforcer painting can create demand where no demand existed, but it is clipped
to the current island. Blocker painting changes only existing demand. Enforcers
are applied before blockers, so a blocker can suppress demand created by an
enforcer in the same layer. The small blocker offset prevents boundary-touching
polygons from surviving a boolean difference because of rounding differences.

The projected painting is computed once per object and reused for all islands
at the corresponding layer. The resulting polygon collection replaces the
host-owned demand entry for that island. This stage modifies support demand
only; it does not generate or remove support extrusions directly.
*/

namespace slic3r_api { namespace Support { namespace SupportDemandPaintingPlugin {

namespace {

const char *k_support_demand_painting_id = "support.demand.painting";
const char *k_dependencies[] = { nullptr };

bool object_has_support_painting(const Object &object)
{
    for (uint32_t volume_idx = 0; volume_idx < object.volume_count(); ++volume_idx)
        if (object.volume(volume_idx).has_painting(RAW_FACET_PAINTING_FDM_SUPPORT))
            return true;
    return false;
}

uint32_t island_work_count(const Object &object)
{
    uint32_t count = 0;
    for (uint32_t layer_idx = 0; layer_idx < object.layer_count(); ++layer_idx)
        count += object.layer(layer_idx).island_count();
    return count;
}

bool has_layer_painting(const std::vector<StoredPolygonCollection> &by_layer, uint32_t layer_idx)
{
    return layer_idx < by_layer.size() && !by_layer[layer_idx].empty();
}

void set_demand_from_operand(const run_ctx_support_demand &ctx,
                             const LayerIsland &island,
                             ClipperOperand &&operand)
{
    StoredExPolygonCollection polygons = operand.to_expolygon_collection();
    polygons.ensure_valid();
    ctx.set(ctx.demand, island.handle(), polygons.mutable_handle());
}

void add_painting_to_island(const run_ctx_support_demand &ctx,
                            const LayerIsland &island,
                            const ClipperContext &clipper,
                            const std::vector<StoredPolygonCollection> &painting,
                            uint32_t layer_idx)
{
    if (!has_layer_painting(painting, layer_idx))
        return;

    ClipperOperand enforced = clipper_intersection(clipper(island.slice()), clipper(painting[layer_idx]));
    if (enforced.empty())
        return;

    expolygon_collection_handle *existing_handle = ctx.get(ctx.demand, island.handle());
    if (existing_handle != nullptr) {
        ExPolygonCollection existing(existing_handle);
        enforced = clipper_union2(clipper(existing), enforced);
    }
    set_demand_from_operand(ctx, island, std::move(enforced));
}

void remove_painting_from_island(const run_ctx_support_demand &ctx,
                                 const LayerIsland &island,
                                 const ClipperContext &clipper,
                                 const std::vector<StoredPolygonCollection> &painting,
                                 uint32_t layer_idx)
{
    if (!has_layer_painting(painting, layer_idx))
        return;

    expolygon_collection_handle *existing_handle = ctx.get(ctx.demand, island.handle());
    if (existing_handle == nullptr)
        return;

    ExPolygonCollection existing(existing_handle);
    ClipperOperand blocked = clipper_offset(clipper(painting[layer_idx]), 1000. * double(SCALED_EPSILON));
    ClipperOperand remaining = clipper_diff(clipper(existing), blocked);
    set_demand_from_operand(ctx, island, std::move(remaining));
}

} // namespace

SupportDemandPainting &
SupportDemandPainting::instance(orchestrator_handle *orch)
{
    static SupportDemandPainting s_instance(orch);
    return s_instance;
}

const char *SupportDemandPainting::id_impl() const noexcept
{
    return k_support_demand_painting_id;
}

const char *SupportDemandPainting::name_impl() const noexcept
{
    return "Support demand painting";
}

const char *SupportDemandPainting::description_impl() const noexcept
{
    return "Apply support enforcer and blocker painting to support demand polygons.";
}

slicing_step_t SupportDemandPainting::step_impl() const noexcept
{
    return STEP_SUPPORT_DEMAND;
}

const char *const *SupportDemandPainting::dependencies_impl() const noexcept
{
    return k_dependencies;
}

int32_t SupportDemandPainting::priority_impl() const noexcept
{
    return 5;
}

const char *SupportDemandPainting::progress_message_format_impl() const noexcept
{
    return "Support demand painting: %u / %u islands";
}

void SupportDemandPainting::setup_run_impl(const plugin_run_context *run_ctx) const
{
    const run_ctx_support_demand *ctx = plugin_ctx_as_support_demand(run_ctx);
    if (ctx == nullptr || ctx->object == nullptr)
        return;

    const Object object(ctx->object);
    if (object_has_support_painting(object))
        progress().add_max(island_work_count(object));
}

void SupportDemandPainting::run_impl(const plugin_run_context *run_ctx) const
{
    const run_ctx_support_demand *ctx = plugin_ctx_as_support_demand(run_ctx);
    if (ctx == nullptr || ctx->object == nullptr || ctx->demand == nullptr)
        return;

    const Object object(ctx->object);
    if (!object_has_support_painting(object)) {
        progress().finish_run();
        return;
    }

    storage_handle *storage = run_ctx->plugin_storage;
    std::vector<StoredPolygonCollection> enforcers = project_painting_to_polygons(
        storage, object, RAW_FACET_PAINTING_FDM_SUPPORT, RAW_FACET_PAINTING_ENFORCER);
    std::vector<StoredPolygonCollection> blockers = project_painting_to_polygons(
        storage, object, RAW_FACET_PAINTING_FDM_SUPPORT, RAW_FACET_PAINTING_BLOCKER);

    ClipperContext clipper(storage);
    for (uint32_t layer_idx = 0; layer_idx < object.layer_count(); ++layer_idx) {
        throw_if_cancelled(run_ctx);

        const Layer layer = object.layer(layer_idx);
        for (uint32_t island_idx = 0; island_idx < layer.island_count(); ++island_idx) {
            throw_if_cancelled(run_ctx);

            const LayerIsland island = layer.island(island_idx);
            add_painting_to_island(*ctx, island, clipper, enforcers, layer_idx);
            remove_painting_from_island(*ctx, island, clipper, blockers, layer_idx);
            progress().increment();
        }
    }

    progress().finish_run();
}

void register_support_demand_painting_plugin(orchestrator_handle *orch)
{
    orchestrator_register_plugin(orch, SupportDemandPainting::instance(orch).c_instance());
}

}}} // namespace slic3r_api::Support::SupportDemandPaintingPlugin

#ifdef SUPPORT_DEMAND_PAINTING_PLUGIN_DLL
extern "C" void register_plugin(orchestrator_handle *orch)
{
    slic3r_api::Support::SupportDemandPaintingPlugin::register_support_demand_painting_plugin(orch);
}
#endif // SUPPORT_DEMAND_PAINTING_PLUGIN_DLL
