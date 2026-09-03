///|/ Copyright (c) SuperSlicer 2026 Durand Remi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/

#include "DefaultInfillGenerator.hpp"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <string>

#include "libslic3r/Api/plugin/c/slic3r_orchestrator.h"
#include "libslic3r/Api/plugin/c/steps/slic3r_step_infill.h"
#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/ExtrusionEntityCollection.hpp"
#include "libslic3r/ExtrusionProperty.hpp"
#include "libslic3r/Config/FFFPrintConfig.hpp"
#include "libslic3r/Layer.hpp"
#include "libslic3r/LayerRegion.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintObject.hpp"
#include "libslic3r/PrintRegion.hpp"
#include "libslic3r/Surface.hpp"
#include "libslic3r/libslic3r.h"

/*
Default infill generation
==========================

This plugin provides the default implementation of STEP_INFILL. The host runs
it once per PrintObject after earlier steps have classified the layer-region
islands into fill surfaces. It does not choose one fixed infill algorithm:
each surface selects a pattern plugin through its configured pattern id.

setup_run_impl() counts the surfaces that may receive infill so progress can
be reported before generation starts. run_impl() then visits layers, islands,
region islands, and their fill surfaces. For each usable surface,
make_recipe() resolves its flow, density, pattern, connection, bridge data,
and overlap area. The optional recipe callback may change those values before
the selected pattern plugin generates an extrusion subtree.

The generated subtree is kept only when the pattern and density are valid. It
receives an ExtrusionPropertyInfill containing the source surface id, then the
plugin publishes it through append_region_island_extrusion(). Gap fill is not
generated here; it is deliberately left for the post-infill step.

The normal call flow is:

    DefaultInfillGenerator::setup_run_impl()
    `-- count_candidate_surfaces(object)
        `-- publish the progress total

    DefaultInfillGenerator::run_impl()
    `-- walk object.layers()
        `-- walk layer.islands()
            `-- walk island.regions_islands()
                `-- walk region_island.fill_surfaces()
                    |-- skip empty or void surfaces
                    |-- make_recipe(...)
                    |   `-- resolve surface settings and no-overlap areas
                    |-- resolve_pattern_id()
                    |-- modify_surface_recipe() when provided
                    |-- generate_pattern()
                    |-- attach ExtrusionPropertyInfill::source_surface_id
                    `-- append_region_island_extrusion()

The generator works through the public step context and callbacks. It reads
the existing print data but leaves ownership and direct publication of the
result to the host API.
*/

namespace slic3r_api { namespace Infill { namespace DefaultInfillGeneratorPlugin {
namespace {

const char *k_default_infill_generator_id = "infill.generator.default";
const char *k_no_dependencies[] = { nullptr };

struct SurfaceInfillRecipe
{
    std::string pattern_plugin_id;
    raw_infill_pattern_params raw = {};
    Slic3r::ExPolygons no_overlap_areas;
};

c_flow to_c_flow(const Slic3r::Flow &flow)
{
    c_flow out = {};
    out.width = flow.scaled_width();
    out.spacing = flow.scaled_spacing();
    out.height = flow.scaled_height();
    out.nozzle_diameter = Slic3r::scale_i(flow.nozzle_diameter());
    out.is_bridge = flow.bridge() ? 1 : 0;
    out.spacing_ratio = flow.spacing_ratio();
    out.mm3_per_mm = flow.mm3_per_mm();
    return out;
}

std::string serialized_pattern_id(Slic3r::InfillPattern pattern)
{
    Slic3r::ConfigOptionEnum<Slic3r::InfillPattern> option(pattern);
    std::string value = option.serialize();
    if (!value.empty() && value.front() == '!')
        value.erase(value.begin());
    return value;
}

float compute_fill_angle_for_layer(const Slic3r::PrintRegionConfig &region_config, size_t layer_id)
{
    // Pattern plugins receive the final angle in radians. Keeping the template
    // and increment logic here avoids duplicating config interpretation in
    // every pattern implementation.
    float angle_degrees = 0.f;
    if (!region_config.fill_angle_template.empty()) {
        const size_t idx = layer_id % region_config.fill_angle_template.size();
        angle_degrees = region_config.fill_angle_template.get_at(idx);
    } else {
        angle_degrees = region_config.fill_angle.value;
    }
    angle_degrees += region_config.fill_angle_increment.value * layer_id;
    return float(Slic3r::Geometry::deg2rad(angle_degrees));
}

Slic3r::FlowRole flow_role_for_surface(const Slic3r::Surface &surface)
{
    if (surface.has_pos_top())
        return Slic3r::frTopSolidInfill;
    if (surface.has_fill_solid())
        return Slic3r::frSolidInfill;
    return Slic3r::frInfill;
}

raw_extrusion_role raw_role_for_surface(const Slic3r::Surface &surface, bool bridge)
{
    if (bridge)
        return surface.has_pos_bottom() ? RAW_EXTRUSION_ROLE_BRIDGE_INFILL :
                                          RAW_EXTRUSION_ROLE_INTERNAL_BRIDGE_INFILL;
    if (surface.has_fill_solid())
        return surface.has_pos_top() ? RAW_EXTRUSION_ROLE_TOP_SOLID_INFILL :
                                       RAW_EXTRUSION_ROLE_SOLID_INFILL;
    return RAW_EXTRUSION_ROLE_INTERNAL_INFILL;
}

Slic3r::InfillPattern pattern_for_surface(const Slic3r::Surface &surface,
                                          bool bridge,
                                          const Slic3r::PrintRegionConfig &region_config)
{
    if (bridge)
        return region_config.bridge_fill_pattern.value;
    if (surface.has_pos_top())
        return region_config.top_fill_pattern.value;
    if (surface.has_pos_bottom())
        return region_config.bottom_fill_pattern.value;
    if (surface.has_fill_solid())
        return region_config.solid_fill_pattern.value;
    return region_config.fill_pattern.value;
}

Slic3r::InfillConnection connection_for_surface(const Slic3r::Surface &surface,
                                                bool bridge,
                                                const Slic3r::PrintRegionConfig &region_config)
{
    if (bridge)
        return region_config.infill_connection_bridge.value;
    if (surface.has_pos_top())
        return region_config.infill_connection_top.value;
    if (surface.has_pos_bottom())
        return region_config.infill_connection_bottom.value;
    if (surface.has_fill_solid())
        return region_config.infill_connection_solid.value;
    return region_config.infill_connection.value;
}

Slic3r::Flow bridge_flow_for_surface(const Slic3r::Layer &layer,
                                     const Slic3r::LayerRegion &region,
                                     Slic3r::FlowRole extrusion_role,
                                     const Slic3r::PrintRegionConfig &region_config)
{
    const float nozzle_diameter =
        layer.object()->print()->config().nozzle_diameter.get_at(
            region.region().extruder(extrusion_role, *layer.object()) - 1);
    double diameter = 0.;
    if (region_config.bridge_type == Slic3r::BridgeType::btFromFlow) {
        const Slic3r::Flow reference_flow = region.flow(Slic3r::frSolidInfill);
        diameter = std::sqrt(4. * reference_flow.mm3_per_mm() / PI);
    } else if (region_config.bridge_type == Slic3r::BridgeType::btFromHeight) {
        diameter = layer.unscaled_height();
    } else {
        diameter = nozzle_diameter;
    }
    return Slic3r::Flow::bridging_flow(
        float(diameter * std::sqrt(region_config.bridge_flow_ratio.get_effective_value(1))),
        nozzle_diameter);
}

Slic3r::Flow normal_flow_for_surface(const Slic3r::Layer &layer,
                                     const Slic3r::LayerRegion &region,
                                     Slic3r::FlowRole extrusion_role,
                                     const Slic3r::Surface &surface)
{
    const float height = surface.scaled_thickness() == -1 ?
        float(layer.unscaled_height()) :
        float(surface.unscaled_thickness());
    return region.region().flow(*layer.object(), extrusion_role, height, layer.id());
}

double perimeter_spacing_for_overlap(const Slic3r::LayerRegion &region,
                                     const Slic3r::PrintRegionConfig &region_config)
{
    if (region_config.perimeters == 1)
        return region.flow(Slic3r::frExternalPerimeter).spacing();
    if (region_config.only_one_perimeter_top)
        return std::min(region.flow(Slic3r::frPerimeter).spacing(),
                        region.flow(Slic3r::frExternalPerimeter).spacing());
    return region.flow(Slic3r::frPerimeter).spacing();
}

Slic3r::ExPolygons make_no_overlap_areas(const Slic3r::LayerSliceIsland &island,
                                         const Slic3r::Surface &surface,
                                         const Slic3r::PrintRegionConfig &region_config,
                                         double overlap)
{
    if (region_config.perimeters <= 0 || overlap == 0.)
        return Slic3r::ExPolygons{surface.expolygon};

    // Pattern extrusion may encroach into perimeters, but volume accounting and
    // bridge placement still need the strictly free area. The island owns that
    // free area because region-level free areas can overlap after offsetting.
    return Slic3r::intersection_ex(island.infill_free_areas(), Slic3r::ExPolygons{surface.expolygon});
}

SurfaceInfillRecipe make_recipe(const Slic3r::Print &print,
                                const Slic3r::PrintObject &object,
                                const Slic3r::Layer &layer,
                                const Slic3r::LayerSliceIsland &island,
                                const Slic3r::LayerRegion &region,
                                const Slic3r::Surface &surface)
{
    const Slic3r::PrintRegionConfig &region_config = region.region().config();
    const bool bridge = layer.id() > 0 && surface.has_mod_bridge();
    const Slic3r::FlowRole extrusion_role = flow_role_for_surface(surface);
    const Slic3r::InfillPattern pattern = pattern_for_surface(surface, bridge, region_config);
    const Slic3r::Flow flow = bridge ?
        bridge_flow_for_surface(layer, region, extrusion_role, region_config) :
        normal_flow_for_surface(layer, region, extrusion_role, surface);

    SurfaceInfillRecipe recipe;
    recipe.pattern_plugin_id = serialized_pattern_id(pattern);
    recipe.raw.surface_type = static_cast<raw_surface_type>(surface.surface_type);
    recipe.raw.extrusion_role = raw_role_for_surface(surface, bridge);
    recipe.raw.flow = to_c_flow(flow);
    recipe.raw.density = surface.has_fill_solid() || bridge ? 1.f : float(region_config.fill_density) / 100.f;
    if (surface.has_mod_overBridge())
        recipe.raw.density = float(region_config.over_bridge_flow_ratio.get_effective_value(1));
    recipe.raw.flow_mult = surface.has_pos_top() ?
        float(region_config.fill_top_flow_ratio.get_effective_value(1)) :
        1.f;
    recipe.raw.connection = int32_t(connection_for_surface(surface, bridge, region_config));

    // Gap fill is now a post-infill responsibility. Pattern plugins should
    // generate their normal fill only and leave narrow residuals for the later
    // post-process plugin.
    recipe.raw.add_gap_fill = 0;
    recipe.raw.gap_fill_enabled = 0;

    recipe.raw.dont_adjust = 0;
    recipe.raw.monotonic = pattern == Slic3r::ipMonotonic || pattern == Slic3r::ipMonotonicLines ? 1 : 0;
    recipe.raw.fill_exactly = region_config.enforce_full_fill_volume.get_bool() ? 1 : 0;
    recipe.raw.complete = 0;
    recipe.raw.use_arachne = (region_config.perimeter_generator == Slic3r::PerimeterGeneratorType::Arachne &&
                              pattern == Slic3r::ipConcentric) ||
                             pattern == Slic3r::ipEnsuring ? 1 : 0;
    recipe.raw.can_angle_cross = region_config.fill_angle_cross ? 1 : 0;
    recipe.raw.extruder = region.region().extruder(extrusion_role, object);
    recipe.raw.priority = int32_t(surface.priority);
    recipe.raw.spacing = bridge || surface.has_fill_solid() ?
        flow.spacing() :
        region.region().flow(object, Slic3r::frInfill, float(layer.unscaled_height()), layer.id()).spacing();
    recipe.raw.angle = compute_fill_angle_for_layer(region_config, layer.id());
    recipe.raw.bridge_angle = surface.bridge_angle;
    recipe.raw.bridge_type = int32_t(static_cast<uint8_t>(region_config.bridge_type.value));
    recipe.raw.layer_height = layer.unscaled_height();
    recipe.raw.z = layer.unscaled_print_z();
    recipe.raw.layer_id = uint32_t(layer.id() - object.layer(0).id());
    recipe.raw.bridge_offset = -1;
    recipe.raw.fill_resolution = std::min(flow.scaled_width() / 16,
        std::max(SCALED_EPSILON, Slic3r::scale_i(print.config().resolution_internal.value)));
    recipe.raw.anchor_length = 1000.f;
    recipe.raw.anchor_length_max = 1000.f;

    if (!surface.has_fill_solid() && !bridge) {
        recipe.raw.anchor_length = float(region_config.infill_anchor);
        if (region_config.infill_anchor.percent)
            recipe.raw.anchor_length = float(recipe.raw.anchor_length * 0.01 * recipe.raw.spacing);
        recipe.raw.anchor_length_max = float(region_config.infill_anchor_max);
        if (region_config.infill_anchor_max.percent)
            recipe.raw.anchor_length_max = float(recipe.raw.anchor_length_max * 0.01 * recipe.raw.spacing);
        recipe.raw.anchor_length = std::min(recipe.raw.anchor_length, recipe.raw.anchor_length_max);
        if (region_config.fill_aligned_z)
            recipe.raw.max_sparse_infill_spacing = Slic3r::unscaled(object.get_sparse_max_spacing());
    } else if (bridge) {
        recipe.raw.anchor_length = 0.f;
        recipe.raw.anchor_length_max = 0.f;
    }

    const double perimeter_spacing = perimeter_spacing_for_overlap(region, region_config);
    recipe.raw.overlap = region_config.perimeters > 0 ?
        region_config.infill_overlap.get_effective_value((perimeter_spacing + recipe.raw.spacing) / 2.) :
        0.;
    const int extruder_index = recipe.raw.extruder == 0 ? 0 : int(recipe.raw.extruder - 1);
    recipe.raw.loop_clipping =
        Slic3r::scale_i(region_config.get_computed_value("seam_gap", extruder_index) * flow.nozzle_diameter());
    recipe.raw.link_max_length = (!flow.bridge() && recipe.raw.density > .8f) ?
        Slic3r::scale_i(3. * recipe.raw.spacing) :
        0;
    recipe.no_overlap_areas = make_no_overlap_areas(island, surface, region_config, recipe.raw.overlap);

    return recipe;
}

const Slic3r::LayerRegion *primary_region(const Slic3r::LayerRegionIsland &region_island)
{
    if (region_island.regions().empty())
        return nullptr;
    return *region_island.regions().begin();
}

bool surface_needs_infill(const Slic3r::Surface &surface)
{
    return !surface.empty() && !surface.has_fill_void();
}

uint32_t count_candidate_surfaces(const Slic3r::PrintObject &object)
{
    uint32_t count = 0;
    for (const Slic3r::Layer &layer : object.layers())
        for (const Slic3r::LayerSliceIsland &island : layer.islands())
            for (const Slic3r::LayerRegionIsland &region_island : island.regions_islands())
                for (const Slic3r::Surface &surface : region_island.fill_surfaces())
                    if (surface_needs_infill(surface))
                        ++count;
    return count;
}

void generate_surface(const run_ctx_generate_infill &ctx,
                      const Slic3r::Print &print,
                      const Slic3r::PrintObject &object,
                      const Slic3r::Layer &layer,
                      const Slic3r::LayerSliceIsland &island,
                      const Slic3r::LayerRegionIsland &region_island,
                      const Slic3r::LayerRegion &region,
                      const Slic3r::Surface &surface)
{
    SurfaceInfillRecipe recipe = make_recipe(print, object, layer, island, region, surface);

    // Config stores the selected pattern as a stable plugin id string. The
    // infill ABI uses a compact runtime id so recipe modifiers can switch
    // patterns without editing caller-owned text buffers.
    recipe.raw.pattern_id = ctx.resolve_pattern_id != nullptr ?
        ctx.resolve_pattern_id(&ctx, recipe.pattern_plugin_id.c_str()) :
        INFILL_PATTERN_RUNTIME_ID_INVALID;
    if (ctx.modify_surface_recipe != nullptr) {
        ctx.modify_surface_recipe(&ctx,
                                  reinterpret_cast<const layer_handle *>(&layer),
                                  reinterpret_cast<const layer_island_handle *>(&island),
                                  reinterpret_cast<const layer_region_island_handle *>(&region_island),
                                  reinterpret_cast<const layer_region_handle *>(&region),
                                  reinterpret_cast<const surface_handle *>(&surface),
                                  reinterpret_cast<const expolygon_collection_handle *>(&recipe.no_overlap_areas),
                                  &recipe.raw);
    }
    if (recipe.raw.pattern_id == INFILL_PATTERN_RUNTIME_ID_INVALID || recipe.raw.density <= 0.f)
        return;

    Slic3r::ExtrusionEntityCollection output;
    output.set_can_sort_reverse(false, false);

    const int32_t generated = ctx.generate_pattern(&ctx,
                                                   recipe.raw.pattern_id,
                                                   reinterpret_cast<const layer_handle *>(&layer),
                                                   reinterpret_cast<const layer_island_handle *>(&island),
                                                   reinterpret_cast<const layer_region_island_handle *>(&region_island),
                                                   reinterpret_cast<const layer_region_handle *>(&region),
                                                   reinterpret_cast<const surface_handle *>(&surface),
                                                   reinterpret_cast<const expolygon_collection_handle *>(&recipe.no_overlap_areas),
                                                   &recipe.raw,
                                                   reinterpret_cast<extrusion_entity_handle *>(&output));
    if (!generated || output.empty())
        return;

    // Keep a direct link from the extrusion subtree to the Surface recipe that
    // produced it. The property is inherited by every child path, so later
    // post-infill plugins can split or reorder paths and still recover the
    // source surface id from the closest parent.
    output.get_or_add_property<Slic3r::ExtrusionPropertyInfill>().source_surface_id = surface.id();

    // The data tree is read-only from the plugin point of view, but this host
    // callback is explicitly the publication channel. The const_cast is limited
    // to building the opaque handle expected by that callback; the plugin never
    // mutates the LayerRegionIsland directly.
    ctx.append_region_island_extrusion(
        reinterpret_cast<layer_region_island_handle *>(const_cast<Slic3r::LayerRegionIsland *>(&region_island)),
        recipe.raw.extrusion_role,
        reinterpret_cast<extrusion_entity_handle *>(&output));
}

} // namespace

DefaultInfillGenerator &
DefaultInfillGenerator::instance(orchestrator_handle *orch)
{
    static DefaultInfillGenerator s_instance(orch);
    return s_instance;
}

const char *DefaultInfillGenerator::id_impl() const noexcept
{
    return k_default_infill_generator_id;
}

const char *DefaultInfillGenerator::name_impl() const noexcept
{
    return "Default infill generator";
}

const char *DefaultInfillGenerator::description_impl() const noexcept
{
    return "Builds infill recipes from LayerRegionIsland surfaces and delegates pattern extrusion to active infill pattern plugins.";
}

slicing_step_t DefaultInfillGenerator::step_impl() const noexcept
{
    return STEP_INFILL;
}

const char *const *DefaultInfillGenerator::dependencies_impl() const noexcept
{
    return k_no_dependencies;
}

int32_t DefaultInfillGenerator::priority_impl() const noexcept
{
    return 0;
}

const char *DefaultInfillGenerator::progress_message_format_impl() const noexcept
{
    return "Generating infill: %u / %u surfaces";
}

void DefaultInfillGenerator::setup_run_impl(const plugin_run_context *run_ctx) const
{
    const run_ctx_generate_infill *ctx = plugin_ctx_as_generate_infill(run_ctx);
    if (ctx == nullptr || ctx->object == nullptr)
        return;

    const Slic3r::PrintObject *object = reinterpret_cast<const Slic3r::PrintObject *>(ctx->object);
    progress().add_max(count_candidate_surfaces(*object));
}

void DefaultInfillGenerator::run_impl(const plugin_run_context *run_ctx) const
{
    const run_ctx_generate_infill *ctx = plugin_ctx_as_generate_infill(run_ctx);
    assert(ctx != nullptr);
    if (ctx == nullptr || ctx->print == nullptr || ctx->object == nullptr ||
        ctx->generate_pattern == nullptr || ctx->append_region_island_extrusion == nullptr) {
        return;
    }

    const Slic3r::Print &print = *reinterpret_cast<const Slic3r::Print *>(ctx->print);
    const Slic3r::PrintObject &object = *reinterpret_cast<const Slic3r::PrintObject *>(ctx->object);
    for (const Slic3r::Layer &layer : object.layers()) {
        for (const Slic3r::LayerSliceIsland &island : layer.islands()) {
            for (const Slic3r::LayerRegionIsland &region_island : island.regions_islands()) {
                const Slic3r::LayerRegion *region = primary_region(region_island);
                if (region == nullptr)
                    continue;

                for (const Slic3r::Surface &surface : region_island.fill_surfaces()) {
                    if (!surface_needs_infill(surface))
                        continue;

                    throw_if_cancelled(run_ctx);
                    generate_surface(*ctx, print, object, layer, island, region_island, *region, surface);
                    progress().increment();
                }
            }
        }
    }
}

void register_default_infill_generator_plugin(orchestrator_handle *orch)
{
    orchestrator_register_plugin(orch, DefaultInfillGenerator::instance(orch).c_instance());
}

}}} // namespace slic3r_api::Infill::DefaultInfillGeneratorPlugin
