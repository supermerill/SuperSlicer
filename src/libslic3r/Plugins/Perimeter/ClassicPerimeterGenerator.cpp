///|/ Copyright (c) SuperSlicer 2026 Durand Rémi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/

#include "ClassicPerimeterGenerator.hpp"

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <vector>

#include "libslic3r/Api/plugin/c/slic3r_orchestrator.h"
#include "libslic3r/Api/plugin/c/steps/slic3r_step_perimeter.h"
#include "libslic3r/Api/plugin/cpp/ClipperViews.hpp"
#include "libslic3r/Api/plugin/cpp/DataTreeViews.hpp"
#include "libslic3r/Api/plugin/cpp/ExtrusionViews.hpp"
#include "libslic3r/Api/plugin/cpp/PerimeterStepViews.hpp"

namespace slic3r_api { namespace Perimeter { namespace ClassicPerimeterGeneratorPlugin {

namespace {

const char *k_classic_perimeter_generator_id = "perimeter.generator.classic";
const char *k_no_dependencies[] = { nullptr };
const raw_used_config_key k_used_config_keys[] = {
    { "perimeters", RAW_CO_INT, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE }
};

constexpr uint16_t k_perimeter_flags_loop = uint16_t(C_EXTRUSION_PERIMETER_FLAG_LOOP);
constexpr uint16_t k_perimeter_flags_hole = uint16_t(C_EXTRUSION_PERIMETER_FLAG_LOOP | C_EXTRUSION_PERIMETER_FLAG_HOLE);

struct ClassicGeneratorState
{
    c_flow external_flow = {};
    c_flow perimeter_flow = {};
    uint32_t perimeter_count = 1;
};

template<class Payload>
Payload &get_or_add_property(StoredExtrusionEntity &entity)
{
    ExtrusionPropertyMutableApi<StoredExtrusionEntity> &properties = entity;
    return properties.template get_or_add_property<Payload>();
}

StoredExPolygonCollection offset_area(storage_handle *storage, const ExPolygon &area, double delta)
{
    ClipperOperand subject(storage, area);
    StoredExPolygonCollection out = clipper_offset(subject, delta).to_expolygon_collection();
    out.ensure_valid();
    return out;
}

void append_classic_loop(StoredExtrusionEntity &dst,
                         const Polygon &polygon,
                         const c_flow &flow,
                         raw_extrusion_role role,
                         uint16_t perimeter_idx,
                         uint16_t perimeter_flags)
{
    if (!polygon.valid_polygon() || polygon.empty())
        return;

    std::vector<c_point> points = polygon.points();
    if (points.empty())
        return;
    points.push_back(points.front());

    StoredExtrusionEntity path(dst.storage(), points);
    EPropertyAttributes &attributes = get_or_add_property<EPropertyAttributes>(path);
    attributes.extrusion_role(role)
        .mm3_per_mm(flow.mm3_per_mm)
        .width(float(unscaled(flow.width)))
        .height(float(unscaled(flow.height)));

    StoredExtrusionEntity loop(dst.storage());
    get_or_add_property<EPropertyPerimeter>(loop).shell_count(perimeter_idx).perimeter_flags(perimeter_flags);
    loop.add_child(path.mutable_view());
    loop.set_flags(RAW_EXTRUSION_FLAG_CONTINUOUS | RAW_EXTRUSION_FLAG_REVERSIBLE);
    dst.add_child(loop.mutable_view());
}

StoredExtrusionEntity make_classic_perimeter_extrusion(storage_handle *storage,
                                                       const ExPolygon &area,
                                                       const c_flow &flow,
                                                       raw_extrusion_role role,
                                                       uint16_t perimeter_idx,
                                                       double line_offset)
{
    StoredExtrusionEntity extrusion(storage);
    extrusion.disable_reverse().disable_sort();

    StoredExPolygonCollection loops = offset_area(storage, area, line_offset);
    for (ExPolygon loop : loops) {
        append_classic_loop(extrusion, loop.contour(), flow, role, perimeter_idx, k_perimeter_flags_loop);
        for (Polygon hole : loop.holes())
            append_classic_loop(extrusion, hole, flow, role, perimeter_idx, k_perimeter_flags_hole);
    }
    return extrusion;
}

void generate_thin_wall(const ClassicGeneratorState &state,
                        const PerimeterNodeView &node,
                        StoredExtrusionEntity &extrusion)
{
    (void)state;
    (void)node;
    (void)extrusion;

    /*
    Future transcription of the thin-wall block from process_classic().

    Legacy flow to preserve when the required API exists:
    - look for thin walls only inside the areas where the thin_walls setting is
      active;
    - use thin_walls_min_width to build the "half_thins" geometry;
    - compute a bit of overlap to anchor thin walls inside the print;
    - clip the no-thin zone with the bounding box of the expanded thin wall, as
      the no-thin zone may be much larger than the local candidate;
    - run the MedialAxis builder with the local bounds, min real width, tapers,
      and min length;
    - store the generated boundary so the normal perimeter area can be reduced
      by the material printed as thin wall.

    TODO: expose the missing plugin-side MedialAxis / thin-wall helpers before
    moving this block. This skeleton must stay API-only and must not include
    host-only geometry classes to get thin walls working prematurely.
    */
}

int32_t generate_one_perimeter(void *generator_context,
                               perimeter_generation_context *context,
                               perimeter_node *node,
                               expolygon_collection_handle *inner_areas_out,
                               expolygon_collection_handle *inner_fill_areas_out)
{
    if (generator_context == nullptr || context == nullptr || context->run_ctx == nullptr ||
        context->run_ctx->plugin_storage == nullptr || node == nullptr ||
        inner_areas_out == nullptr || inner_fill_areas_out == nullptr)
        return 0;

    const ClassicGeneratorState &state = *reinterpret_cast<const ClassicGeneratorState *>(generator_context);
    storage_handle *storage = context->run_ctx->plugin_storage;
    PerimeterNodeView node_view(node);

    /*
    process_classic() used one mutable "last" polygon set for the current
    onion-shell area. In the new pipeline the host perimeter tree stores that
    state per node: node.area() is the current "last", node.fill_area() is the
    infill area saved while perimeter modules continue on other branches.

    Legacy variables to map during the real transfer:
    - last: current area to shrink for this perimeter depth;
    - last_overhang: overhang areas carried while perimeters are generated;
    - gaps: remaining narrow spaces found after shrinking;
    - perimeter_gaps_ex: gap areas turned into perimeter material and removed
      from infill;
    - last_asynch: expolygons where contour and hole growth are intentionally
      different;
    - saved_infill: infill area saved while extra perimeters continue elsewhere.
    */

    if (state.perimeter_count > 0)
        node_view.set_perimeter_needed(std::max(node_view.perimeter_needed(), state.perimeter_count));

    if (!node_view.needs_more_perimeters()) {
        StoredExPolygonCollection inner_areas(storage);
        inner_areas.push_back(node_view.area());
        expolygons_move(inner_areas_out, inner_areas.mutable_handle());

        StoredExPolygonCollection inner_fill_areas(storage);
        inner_fill_areas.push_back(node_view.fill_area());
        expolygons_move(inner_fill_areas_out, inner_fill_areas.mutable_handle());
        return 1;
    }

    /*
    The legacy loop runs one time more than the requested perimeter count to
    find gap-fill areas after the last perimeter was applied. The host loop
    currently calls this callback for actual perimeter nodes only; the future
    classic transfer will need to model the "one extra pass" either as explicit
    gap-fill publication here or as a post-perimeter module.

    The blocks already implemented by separate modules must not be copied back
    here: extra_perimeters_count, extra_perimeters_below_area,
    extra_perimeters_odd_layers, only-one-perimeter variants,
    SeparateHoleContour, overhang post-processing, and fuzzy skin.
    */

    const bool first_perimeter = node_view.perimeter_idx() == 0;
    const c_flow flow = first_perimeter ? state.external_flow : state.perimeter_flow;
    const raw_extrusion_role role = first_perimeter ?
        RAW_EXTRUSION_ROLE_EXTERNAL_PERIMETER :
        RAW_EXTRUSION_ROLE_INTERNAL_PERIMETER;
    const uint16_t perimeter_idx = uint16_t(std::min<uint32_t>(node_view.perimeter_idx(), UINT16_MAX));

    /*
    Calculate next onion shell of perimeters.

    The full classic implementation will reproduce the offset2/asynchronous
    contour-hole logic here. This temporary skeleton intentionally uses the
    simple stable shrink convention so the plugin can be selected and exercised
    before the classic offsets are migrated.
    */
    const double line_offset = first_perimeter ? -0.5 * double(flow.width) :
                                                 -0.5 * double(flow.spacing);
    StoredExtrusionEntity extrusion =
        make_classic_perimeter_extrusion(storage, node_view.area(), flow, role, perimeter_idx, line_offset);
    generate_thin_wall(state, node_view, extrusion);
    extrusion_move_from(node->extrusions, extrusion.mutable_handle());

    /*
    Publish child areas.

    process_classic() computes both the next perimeter area and the infill/gap
    areas derived from it. The host consumes inner_areas_out as child nodes, and
    inner_fill_areas_out as the corresponding fill/free areas. The temporary
    offsets below mirror SimplePerimeterGenerator; they are placeholders for the
    future classic next_onion / saved_infill / perimeter_gaps_ex calculation.
    */
    const double inner_offset = first_perimeter ? -0.5 * double(flow.width + state.perimeter_flow.spacing) :
                                                  -double(state.perimeter_flow.spacing);
    StoredExPolygonCollection inner_areas = offset_area(storage, node_view.area(), inner_offset);
    expolygons_move(inner_areas_out, inner_areas.mutable_handle());

    const double fill_offset = inner_offset + 0.25 * double(state.perimeter_flow.spacing);
    StoredExPolygonCollection inner_fill_areas = offset_area(storage, node_view.area(), fill_offset);
    expolygons_move(inner_fill_areas_out, inner_fill_areas.mutable_handle());

    return 1;
}

c_flow external_perimeter_flow(const LayerIsland &island)
{
    return island.region_count() > 0 ? island.region(0).flow(RAW_EXTRUSION_ROLE_EXTERNAL_PERIMETER) : c_flow{};
}

c_flow perimeter_flow(const LayerIsland &island)
{
    return island.region_count() > 0 ? island.region(0).flow(RAW_EXTRUSION_ROLE_INTERNAL_PERIMETER) : c_flow{};
}

uint32_t perimeter_count(const LayerIsland &island)
{
    if (island.region_count() == 0)
        return 0;
    const int32_t count = island.region(0).print_region().config().get("perimeters").get_int();
    return count <= 0 ? 0 : uint32_t(count);
}

} // namespace

ClassicPerimeterGenerator &
ClassicPerimeterGenerator::instance(orchestrator_handle *orch)
{
    static ClassicPerimeterGenerator s_instance(orch);
    return s_instance;
}

const char *ClassicPerimeterGenerator::id_impl() const noexcept
{
    return k_classic_perimeter_generator_id;
}

const char *ClassicPerimeterGenerator::name_impl() const noexcept
{
    return "Classic perimeter generator";
}

const char *ClassicPerimeterGenerator::description_impl() const noexcept
{
    return "API-only skeleton for the future process_classic perimeter generator.";
}

slicing_step_t ClassicPerimeterGenerator::step_impl() const noexcept
{
    return STEP_PERIMETER;
}

const char *const *ClassicPerimeterGenerator::dependencies_impl() const noexcept
{
    return k_no_dependencies;
}

int32_t ClassicPerimeterGenerator::priority_impl() const noexcept
{
    return 5;
}

int32_t ClassicPerimeterGenerator::used_config_keys(raw_used_config_key *keys) const noexcept
{
    if (keys != nullptr)
        for (uint32_t idx = 0; idx < sizeof(k_used_config_keys) / sizeof(k_used_config_keys[0]); ++idx)
            keys[idx] = k_used_config_keys[idx];
    return int32_t(sizeof(k_used_config_keys) / sizeof(k_used_config_keys[0]));
}

const char *ClassicPerimeterGenerator::progress_message_format_impl() const noexcept
{
    return "Classic perimeter generator: %u / %u islands";
}

void ClassicPerimeterGenerator::setup_run_impl(const plugin_run_context *run_ctx) const
{
    const run_ctx_generate_perimeter *ctx = plugin_ctx_as_generate_perimeter(run_ctx);
    if (ctx != nullptr && ctx->island != nullptr)
        progress().add_max(1);
}

void ClassicPerimeterGenerator::run_impl(const plugin_run_context *run_ctx) const
{
    const run_ctx_generate_perimeter *ctx = plugin_ctx_as_generate_perimeter(run_ctx);
    if (ctx == nullptr || ctx->island == nullptr || ctx->run_region_group == nullptr)
        return;

    throw_if_cancelled(run_ctx);

    const LayerIsland island(ctx->island);
    if (island.region_count() == 0)
        return;

    std::vector<const layer_region_handle *> regions;
    regions.reserve(island.region_count());
    for (uint32_t region_idx = 0; region_idx < island.region_count(); ++region_idx)
        regions.push_back(island.region(region_idx).handle());

    ClassicGeneratorState state;
    state.external_flow = external_perimeter_flow(island);
    state.perimeter_flow = perimeter_flow(island);
    state.perimeter_count = perimeter_count(island);

    ctx->run_region_group(ctx,
                          regions.empty() ? nullptr : regions.data(),
                          uint32_t(regions.size()),
                          island.slice().handle(),
                          &state,
                          &generate_one_perimeter);

    progress().increment();
}

void register_classic_perimeter_generator_plugin(orchestrator_handle *orch)
{
    orchestrator_register_plugin(orch, ClassicPerimeterGenerator::instance(orch).c_instance());
}

}}} // namespace slic3r_api::Perimeter::ClassicPerimeterGeneratorPlugin

#ifdef CLASSIC_PERIMETER_GENERATOR_PLUGIN_DLL
extern "C" void register_plugin(orchestrator_handle *orch)
{
    slic3r_api::Perimeter::ClassicPerimeterGeneratorPlugin::register_classic_perimeter_generator_plugin(orch);
}
#endif // CLASSIC_PERIMETER_GENERATOR_PLUGIN_DLL
