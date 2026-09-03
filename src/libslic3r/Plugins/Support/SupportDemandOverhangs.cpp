///|/ Copyright (c) SuperSlicer 2026 Durand Rémi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/

#include "SupportDemandOverhangs.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

#include "libslic3r/Api/plugin/c/slic3r_orchestrator.h"
#include "libslic3r/Api/plugin/cpp/Views.hpp"

/*
SupportDemandOverhangs
======================

This plugin creates support-demand polygons from the part of each layer that
is not supported by the layer below. It runs at STEP_SUPPORT_DEMAND, before
support geometry is generated, and combines its result with demand already
stored by earlier support-demand plugins.

The normal execution flow is:

    setup_run_impl()
    `-- reserve progress for every island above the first layer

    run_impl()
    |-- read the object support settings
    |-- for each layer from bottom to top, compare it with the layer below
    |-- enlarge the lower-layer coverage according to the overhang threshold
    |   or the external-perimeter width
    |-- subtract that coverage from the current island slice
    `-- union the unsupported area with the island's existing demand

The first layer is skipped because it has no lower layer to provide support.
Automatic demand is generated only when support material and automatic support
are enabled. Enforced-support layers are processed even when automatic support
is disabled. The configured overhang threshold determines the horizontal
allowance when it is valid; otherwise, half the smallest external-perimeter
width of the island is used.

The calculation is performed independently for each layer island. The result
is limited to the current island slice and stored in the host-owned demand
map. This stage identifies areas that need support; it does not generate the
support extrusions themselves.
*/

namespace slic3r_api { namespace Support { namespace SupportDemandOverhangsPlugin {

namespace {

const char *k_support_demand_overhangs_id = "support.demand.overhangs";
const char *k_no_dependencies[] = { nullptr };

struct ObjectSupportDemandConfig
{
    bool support_material = false;
    bool support_material_auto = false;
    int32_t support_material_enforce_layers = 0;
    int32_t support_material_threshold = 0;
};

ObjectSupportDemandConfig read_object_config(const Object &object)
{
    const Config object_config = object.config();
    ObjectSupportDemandConfig out;
    out.support_material = object_config.get("support_material").get_bool();
    out.support_material_auto = object_config.get("support_material_auto").get_bool();
    out.support_material_enforce_layers = object_config.get("support_material_enforce_layers").get_int();
    out.support_material_threshold = object_config.get("support_material_threshold").get_int();
    return out;
}

bool has_auto_support(const ObjectSupportDemandConfig &config)
{
    return config.support_material && config.support_material_auto;
}

bool has_support_demand(const ObjectSupportDemandConfig &config)
{
    return has_auto_support(config) || config.support_material_enforce_layers > 0;
}

uint32_t island_work_count(const Object &object)
{
    uint32_t count = 0;
    for (uint32_t layer_idx = 1; layer_idx < object.layer_count(); ++layer_idx)
        count += object.layer(layer_idx).island_count();
    return count;
}

bool enforced_support_layer(const ObjectSupportDemandConfig &config, uint32_t layer_idx)
{
    return config.support_material_enforce_layers > 0 && layer_idx < uint32_t(config.support_material_enforce_layers);
}

coord_t island_external_perimeter_width(const LayerIsland &island)
{
    coord_t width = std::numeric_limits<coord_t>::max();
    for (uint32_t region_idx = 0; region_idx < island.region_count(); ++region_idx) {
        const c_flow flow = island.region(region_idx).flow(RAW_EXTRUSION_ROLE_EXTERNAL_PERIMETER);
        if (flow.width > 0 && (width == 0 || flow.width < width))
            width = std::min(width, flow.width);
    }
    return width < std::numeric_limits<coord_t>::max() ? width : scale_i(0.4);
}

coord_t lower_layer_offset(const ObjectSupportDemandConfig &config,
                           const Layer &lower_layer,
                           const LayerIsland &island,
                           uint32_t layer_idx)
{
    if (enforced_support_layer(config, layer_idx))
        return 0;

    if (config.support_material_threshold > 0) {
        const double threshold_rad = PI * double(config.support_material_threshold + 1) / 180.;
        if (threshold_rad > 0. && threshold_rad < PI / 2.)
            return scale_to_layer_coord(unscaled(lower_layer.height()) / std::tan(threshold_rad));
    }

    return island_external_perimeter_width(island) / 2;
}

void add_to_demand(const run_ctx_support_demand &ctx,
                   const LayerIsland &island,
                   storage_handle *storage,
                   ClipperOperand &unsupported)
{
    if (unsupported.empty())
        return;

    ClipperContext clipper(storage);
    StoredExPolygonCollection polygons = unsupported.to_expolygon_collection();
    polygons.ensure_valid();

    expolygon_collection_handle *existing = ctx.get(ctx.demand, island.handle());
    if (existing != nullptr) {
        ExPolygonCollection existing_polygons(existing);
        polygons = clipper_union2(clipper(existing_polygons), clipper(polygons)).to_expolygon_collection();
        polygons.ensure_valid();
    }

    ctx.set(ctx.demand, island.handle(), polygons.mutable_handle());
}

} // namespace

SupportDemandOverhangs &
SupportDemandOverhangs::instance(orchestrator_handle *orch)
{
    static SupportDemandOverhangs s_instance(orch);
    return s_instance;
}

const char *SupportDemandOverhangs::id_impl() const noexcept
{
    return k_support_demand_overhangs_id;
}

const char *SupportDemandOverhangs::name_impl() const noexcept
{
    return "Support demand overhangs";
}

const char *SupportDemandOverhangs::description_impl() const noexcept
{
    return "Create support demand polygons from layer overhangs.";
}

slicing_step_t SupportDemandOverhangs::step_impl() const noexcept
{
    return STEP_SUPPORT_DEMAND;
}

const char *const *SupportDemandOverhangs::dependencies_impl() const noexcept
{
    return k_no_dependencies;
}

int32_t SupportDemandOverhangs::priority_impl() const noexcept
{
    return 0;
}

const char *SupportDemandOverhangs::progress_message_format_impl() const noexcept
{
    return "Support demand overhangs: %u / %u islands";
}

void SupportDemandOverhangs::setup_run_impl(const plugin_run_context *run_ctx) const
{
    const run_ctx_support_demand *ctx = plugin_ctx_as_support_demand(run_ctx);
    if (ctx == nullptr || ctx->object == nullptr)
        return;

    const Object object(ctx->object);
    progress().add_max(island_work_count(object));
}

void SupportDemandOverhangs::run_impl(const plugin_run_context *run_ctx) const
{
    const run_ctx_support_demand *ctx = plugin_ctx_as_support_demand(run_ctx);
    if (ctx == nullptr || ctx->object == nullptr || ctx->demand == nullptr)
        return;

    const Object object(ctx->object);
    const ObjectSupportDemandConfig object_config = read_object_config(object);
    if (!has_support_demand(object_config)) {
        progress().finish_run();
        return;
    }

    storage_handle *storage = run_ctx->plugin_storage;
    ClipperContext clipper(storage);
    const bool support_auto = has_auto_support(object_config);

    for (uint32_t layer_idx = 1; layer_idx < object.layer_count(); ++layer_idx) {
        throw_if_cancelled(run_ctx);

        const Layer layer = object.layer(layer_idx);
        const Layer lower_layer = object.layer(layer_idx - 1);
        const ExPolygonCollection lower_slices = lower_layer.slices();
        const bool enforced_layer = enforced_support_layer(object_config, layer_idx);

        for (uint32_t island_idx = 0; island_idx < layer.island_count(); ++island_idx) {
            throw_if_cancelled(run_ctx);

            const LayerIsland island = layer.island(island_idx);
            if (!support_auto && !enforced_layer) {
                progress().increment();
                continue;
            }

            ClipperOperand lower_support = clipper(lower_slices);
            const coord_t offset = lower_layer_offset(object_config, lower_layer, island, layer_idx);
            if (offset > 0)
                lower_support = clipper_offset(lower_support, double(offset));

            ClipperOperand unsupported = clipper_diff_with_safety_offset(clipper(island.slice()), lower_support);
            add_to_demand(*ctx, island, storage, unsupported);
            progress().increment();
        }
    }

    progress().finish_run();
}

void register_support_demand_overhangs_plugin(orchestrator_handle *orch)
{
    orchestrator_register_plugin(orch, SupportDemandOverhangs::instance(orch).c_instance());
}

}}} // namespace slic3r_api::Support::SupportDemandOverhangsPlugin

#ifdef SUPPORT_DEMAND_OVERHANGS_PLUGIN_DLL
extern "C" void register_plugin(orchestrator_handle *orch)
{
    slic3r_api::Support::SupportDemandOverhangsPlugin::register_support_demand_overhangs_plugin(orch);
}
#endif // SUPPORT_DEMAND_OVERHANGS_PLUGIN_DLL
