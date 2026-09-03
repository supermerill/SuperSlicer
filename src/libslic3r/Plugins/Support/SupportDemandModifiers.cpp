///|/ Copyright (c) SuperSlicer 2026 Durand Rémi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/

#include "SupportDemandModifiers.hpp"

#include <algorithm>
#include <cstdint>
#include <utility>
#include <vector>

#include "libslic3r/Api/plugin/c/slic3r_orchestrator.h"
#include "libslic3r/Api/plugin/c/steps/slic3r_step_support_demand.h"
#include "libslic3r/Api/plugin/cpp/Views.hpp"

/*
SupportDemandModifiers
======================

This plugin applies support-enforcer and support-blocker volumes to the
support-demand polygons produced for an object. It works on the demand stage,
before support geometry is generated, so it changes where support may be
requested rather than deleting already generated support extrusions.

The normal execution flow is:

    setup_run_impl()
    `-- reserve progress for every object layer island when a relevant volume exists

    run_impl()
    |-- collect the object's layer Z positions and slicing parameters
    |-- slice relevant modifier volumes in object-volume order
    `-- for every layer island:
        |-- intersect enforcers with the island and union them with existing demand
        `-- subtract blockers from the resulting demand

Each modifier volume is sliced once and its polygons are reused for all layer
islands at the same Z. Enforcers can create demand where none existed, but only
inside the object island. Blockers affect only existing demand. A small offset
is applied to blocker polygons before subtraction so boundary-touching areas
are not left as support demand because of polygon-rounding differences.

The plugin keeps the host-owned demand storage as the source of truth and
replaces an island's polygon collection after each boolean operation. Volume
priority is therefore determined by object-volume order: later modifiers see
the demand produced by earlier ones, and a later blocker can remove an earlier
enforcer's result.
*/

namespace slic3r_api { namespace Support { namespace SupportDemandModifiersPlugin {

namespace {

const char *k_support_demand_modifiers_id = "support.demand.modifiers";
const char *k_dependencies[] = { nullptr };

bool is_support_modifier_type(raw_volume_type type)
{
    return type == RAW_VOLUME_TYPE_SUPPORT_ENFORCER || type == RAW_VOLUME_TYPE_SUPPORT_BLOCKER;
}

bool object_has_support_modifier_volume(const Object &object)
{
    for (uint32_t volume_idx = 0; volume_idx < object.volume_count(); ++volume_idx)
        if (is_support_modifier_type(object.volume(volume_idx).type()))
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

std::vector<float> layer_slice_zs(const Object &object)
{
    std::vector<float> slice_zs;
    slice_zs.reserve(object.layer_count());
    for (uint32_t layer_idx = 0; layer_idx < object.layer_count(); ++layer_idx)
        slice_zs.push_back(float(unscaled(object.layer(layer_idx).slice_z())));
    return slice_zs;
}

c_mesh_slicing_params support_modifier_slicing_params(const Print &print, const Object &object)
{
    const Config print_config = print.config();

    c_mesh_slicing_params params = {};
    params.transform = object.transform_centered();
    params.resolution = std::max(EPSILON, print_config.get("resolution").get_float());
    params.mode = RAW_MESH_SLICING_MODE_REGULAR;
    params.mode_below = params.mode;
    return params;
}

struct SlicedSupportModifier
{
    raw_volume_type type = RAW_VOLUME_TYPE_INVALID;
    std::vector<StoredExPolygonCollection> slices;
};

bool has_any_layer_slices(const std::vector<StoredExPolygonCollection> &by_layer)
{
    for (const StoredExPolygonCollection &layer_slices : by_layer)
        if (!layer_slices.empty())
            return true;
    return false;
}

std::vector<StoredExPolygonCollection> slice_support_modifier_volume(const Volume &volume,
                                                                     const std::vector<float> &slice_zs,
                                                                     const c_mesh_slicing_params &base_params,
                                                                     storage_handle *storage)
{
    std::vector<StoredExPolygonCollection> out;
    const coord_t resolution = scale_i(base_params.resolution);

    if (volume.mesh().empty())
        return out;

    c_mesh_slicing_params params = base_params;
    params.transform = matrix4d_mul(base_params.transform, volume.matrix());
    out = volume.mesh().slice_to_expolygons(storage, params, slice_zs);
    for (StoredExPolygonCollection &layer_polygons : out)
        layer_polygons.ensure_valid(resolution);

    return out;
}

std::vector<SlicedSupportModifier> slice_support_modifier_volumes_in_order(const Object &object,
                                                                           const std::vector<float> &slice_zs,
                                                                           const c_mesh_slicing_params &base_params,
                                                                           storage_handle *storage)
{
    std::vector<SlicedSupportModifier> out;

    for (uint32_t volume_idx = 0; volume_idx < object.volume_count(); ++volume_idx) {
        const Volume volume = object.volume(volume_idx);
        const raw_volume_type type = volume.type();
        if (!is_support_modifier_type(type))
            continue;

        std::vector<StoredExPolygonCollection> slices =
            slice_support_modifier_volume(volume, slice_zs, base_params, storage);
        if (!has_any_layer_slices(slices))
            continue;

        SlicedSupportModifier modifier;
        modifier.type = type;
        modifier.slices = std::move(slices);
        out.emplace_back(std::move(modifier));
    }

    return out;
}

bool has_layer_slices(const std::vector<StoredExPolygonCollection> &by_layer, uint32_t layer_idx)
{
    return layer_idx < by_layer.size() && !by_layer[layer_idx].empty();
}

ClipperOperand clip_layer_slices(const std::vector<StoredExPolygonCollection> &by_layer,
                                 uint32_t layer_idx,
                                 const ClipperContext &clipper)
{
    assert(has_layer_slices(by_layer, layer_idx));
    return clipper(by_layer[layer_idx]);
}

void set_demand_from_operand(const run_ctx_support_demand &ctx,
                             const LayerIsland &island,
                             ClipperOperand &&operand)
{
    StoredExPolygonCollection polygons = operand.to_expolygon_collection();
    polygons.ensure_valid();
    ctx.set(ctx.demand, island.handle(), polygons.mutable_handle());
}

void add_enforcers_to_island(const run_ctx_support_demand &ctx,
                             const LayerIsland &island,
                             const ClipperContext &clipper,
                             const std::vector<StoredExPolygonCollection> &enforcers,
                             uint32_t layer_idx)
{
    if (!has_layer_slices(enforcers, layer_idx))
        return;

    ClipperOperand enforced =
        clipper_intersection(clipper(island.slice()), clip_layer_slices(enforcers, layer_idx, clipper));
    if (enforced.empty())
        return;

    expolygon_collection_handle *existing_handle = ctx.get(ctx.demand, island.handle());
    if (existing_handle != nullptr) {
        ExPolygonCollection existing(existing_handle);
        enforced = clipper_union2(clipper(existing), enforced);
    }
    set_demand_from_operand(ctx, island, std::move(enforced));
}

void remove_blockers_from_island(const run_ctx_support_demand &ctx,
                                 const LayerIsland &island,
                                 const ClipperContext &clipper,
                                 const std::vector<StoredExPolygonCollection> &blockers,
                                 uint32_t layer_idx)
{
    if (!has_layer_slices(blockers, layer_idx))
        return;

    expolygon_collection_handle *existing_handle = ctx.get(ctx.demand, island.handle());
    if (existing_handle == nullptr)
        return;

    ExPolygonCollection existing(existing_handle);
    ClipperOperand blocked =
        clipper_offset(clip_layer_slices(blockers, layer_idx, clipper), 1000. * double(SCALED_EPSILON));
    ClipperOperand remaining = clipper_diff(clipper(existing), blocked);
    set_demand_from_operand(ctx, island, std::move(remaining));
}

} // namespace

SupportDemandModifiers &
SupportDemandModifiers::instance(orchestrator_handle *orch)
{
    static SupportDemandModifiers s_instance(orch);
    return s_instance;
}

const char *SupportDemandModifiers::id_impl() const noexcept
{
    return k_support_demand_modifiers_id;
}

const char *SupportDemandModifiers::name_impl() const noexcept
{
    return "Support demand modifiers";
}

const char *SupportDemandModifiers::description_impl() const noexcept
{
    return "Apply support enforcer and blocker modifier volumes to support demand polygons.";
}

slicing_step_t SupportDemandModifiers::step_impl() const noexcept
{
    return STEP_SUPPORT_DEMAND;
}

const char *const *SupportDemandModifiers::dependencies_impl() const noexcept
{
    return k_dependencies;
}

int32_t SupportDemandModifiers::priority_impl() const noexcept
{
    return 10;
}

const char *SupportDemandModifiers::progress_message_format_impl() const noexcept
{
    return "Support demand modifiers: %u / %u islands";
}

void SupportDemandModifiers::setup_run_impl(const plugin_run_context *run_ctx) const
{
    const run_ctx_support_demand *ctx = plugin_ctx_as_support_demand(run_ctx);
    if (ctx == nullptr || ctx->object == nullptr)
        return;

    const Object object(ctx->object);
    if (object_has_support_modifier_volume(object))
        progress().add_max(island_work_count(object));
}

void SupportDemandModifiers::run_impl(const plugin_run_context *run_ctx) const
{
    const run_ctx_support_demand *ctx = plugin_ctx_as_support_demand(run_ctx);
    if (ctx == nullptr || ctx->print == nullptr || ctx->object == nullptr || ctx->demand == nullptr)
        return;

    const Object object(ctx->object);
    if (!object_has_support_modifier_volume(object)) {
        progress().finish_run();
        return;
    }

    storage_handle *storage = run_ctx->plugin_storage;
    const Print print(ctx->print);
    const std::vector<float> slice_zs = layer_slice_zs(object);
    const c_mesh_slicing_params params = support_modifier_slicing_params(print, object);
    const std::vector<SlicedSupportModifier> modifiers =
        slice_support_modifier_volumes_in_order(object, slice_zs, params, storage);

    if (modifiers.empty()) {
        progress().finish_run();
        return;
    }

    ClipperContext clipper(storage);
    for (uint32_t layer_idx = 0; layer_idx < object.layer_count(); ++layer_idx) {
        throw_if_cancelled(run_ctx);

        const Layer layer = object.layer(layer_idx);
        for (uint32_t island_idx = 0; island_idx < layer.island_count(); ++island_idx) {
            throw_if_cancelled(run_ctx);

            const LayerIsland island = layer.island(island_idx);
            for (const SlicedSupportModifier &modifier : modifiers) {
                if (modifier.type == RAW_VOLUME_TYPE_SUPPORT_ENFORCER)
                    add_enforcers_to_island(*ctx, island, clipper, modifier.slices, layer_idx);
                else if (modifier.type == RAW_VOLUME_TYPE_SUPPORT_BLOCKER)
                    remove_blockers_from_island(*ctx, island, clipper, modifier.slices, layer_idx);
            }
            progress().increment();
        }
    }

    progress().finish_run();
}

void register_support_demand_modifiers_plugin(orchestrator_handle *orch)
{
    orchestrator_register_plugin(orch, SupportDemandModifiers::instance(orch).c_instance());
}

}}} // namespace slic3r_api::Support::SupportDemandModifiersPlugin

#ifdef SUPPORT_DEMAND_MODIFIERS_PLUGIN_DLL
extern "C" void register_plugin(orchestrator_handle *orch)
{
    slic3r_api::Support::SupportDemandModifiersPlugin::register_support_demand_modifiers_plugin(orch);
}
#endif // SUPPORT_DEMAND_MODIFIERS_PLUGIN_DLL
