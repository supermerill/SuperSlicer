///|/ Copyright (c) SuperSlicer 2026 Durand Remi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/

#include "LegacyInfillPatterns.hpp"

#include <cassert>
#include <array>
#include <memory>
#include <stdexcept>
#include <vector>

#include "libslic3r/Api/plugin/c/slic3r_orchestrator.h"
#include "libslic3r/Api/plugin/c/steps/slic3r_step_infill.h"
#include "libslic3r/Fill/FillBase.hpp"
#include "libslic3r/Layer.hpp"
#include "libslic3r/LayerRegion.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintObject.hpp"
#include "libslic3r/PrintRegion.hpp"
#include "libslic3r/Surface.hpp"

/*
Legacy infill-pattern adapters
==============================

This file exposes the existing Fill algorithms through the INFILL_PATTERN
plugin step. One LegacyInfillPattern instance is created for each supported
InfillPattern value; the instance keeps the stable plugin id, display text,
priority, and native pattern enum needed by that algorithm.

The default infill generator builds a raw_infill_pattern_params structure for
one surface and asks the selected pattern plugin to fill it. Each adapter
validates its context, reconstructs Flow and FillParams from the ABI values,
creates the corresponding native Fill object, copies the layer and overlap
data it needs, and calls Fill::fill_surface_extrusion() to append paths to the
provided output collection.

The normal call flow is:

    register_legacy_infill_pattern_plugins()
    `-- legacy_instances()
        `-- create one LegacyInfillPattern per k_legacy_patterns entry
            `-- orchestrator_register_plugin()

    LegacyInfillPattern::run_impl()
    `-- validate the INFILL_PATTERN context
        |-- unsupported legacy object state?
        |   `-- report that this pattern is not available in the new context
        `-- otherwise
            |-- fill_params_from_raw()
            |   `-- rebuild Flow and FillParams
            |-- Fill::new_from_type(m_pattern)
            |-- configure bounds, layer, angle, clipping, and overlap
            |-- Fill::init_spacing()
            `-- Fill::fill_surface_extrusion()

The adapter deliberately preserves the old low-level geometry algorithms.
`ipAdaptiveCubic`, `ipSupportCubic`, and `ipLightning` remain selectable in
the registry, but currently require object-level precomputed data that the
INFILL_PATTERN context does not expose; they fail explicitly instead of
producing incomplete infill.
*/

namespace slic3r_api { namespace Infill { namespace LegacyInfillPatternsPlugin {
namespace {

const char *k_no_dependencies[] = { nullptr };

struct LegacyPatternDefinition
{
    const char *id;
    const char *name;
    const char *description;
    Slic3r::InfillPattern pattern;
};

const std::array<LegacyPatternDefinition, 26> k_legacy_patterns = {{
    {"rectilinear", "Rectilinear", "Classic rectilinear infill pattern.", Slic3r::ipRectilinear},
    {"monotonic", "Monotonic", "Monotonic solid infill pattern.", Slic3r::ipMonotonic},
    {"alignedrectilinear", "Aligned Rectilinear", "Rectilinear infill aligned across layers.", Slic3r::ipAlignedRectilinear},
    {"grid", "Grid", "Grid infill pattern.", Slic3r::ipGrid},
    {"triangles", "Triangles", "Triangular infill pattern.", Slic3r::ipTriangles},
    {"stars", "Stars", "Star-shaped triangular infill pattern.", Slic3r::ipStars},
    {"cubic", "Cubic", "Cubic infill pattern.", Slic3r::ipCubic},
    {"line", "Line", "Simple alternating line infill pattern.", Slic3r::ipLine},
    {"monotoniclines", "Monotonic Lines", "Monotonic line infill pattern.", Slic3r::ipMonotonicLines},
    {"concentric", "Concentric", "Concentric infill pattern.", Slic3r::ipConcentric},
    {"honeycomb", "Honeycomb", "Honeycomb infill pattern.", Slic3r::ipHoneycomb},
    {"3dhoneycomb", "3D Honeycomb", "3D honeycomb infill pattern.", Slic3r::ip3DHoneycomb},
    {"gyroid", "Gyroid", "Gyroid infill pattern.", Slic3r::ipGyroid},
    {"hilbertcurve", "Hilbert Curve", "Hilbert curve infill pattern.", Slic3r::ipHilbertCurve},
    {"archimedeanchords", "Archimedean Chords", "Archimedean chords infill pattern.", Slic3r::ipArchimedeanChords},
    {"octagramspiral", "Octagram Spiral", "Octagram spiral infill pattern.", Slic3r::ipOctagramSpiral},
    {"adaptivecubic", "Adaptive Cubic", "Adaptive cubic infill pattern.", Slic3r::ipAdaptiveCubic},
    {"supportcubic", "Support Cubic", "Support cubic infill pattern.", Slic3r::ipSupportCubic},
    {"smooth", "Ironing", "Ironing-style smooth infill pattern.", Slic3r::ipSmooth},
    {"smoothhilbert", "Smooth Hilbert", "Hilbert-based smooth infill pattern.", Slic3r::ipSmoothHilbert},
    {"smoothtriple", "Smooth Triple", "Triple-pass smooth infill pattern.", Slic3r::ipSmoothTriple},
    {"rectiwithperimeter", "Rectilinear with Perimeter", "Rectilinear infill with perimeter-aware filling.", Slic3r::ipRectiWithPerimeter},
    {"scatteredrectilinear", "Scattered Rectilinear", "Scattered rectilinear infill pattern.", Slic3r::ipScatteredRectilinear},
    {"sawtooth", "Sawtooth", "Sawtooth rectilinear infill pattern.", Slic3r::ipSawtooth},
    {"lightning", "Lightning", "Lightning infill pattern.", Slic3r::ipLightning},
    {"ensuring", "Ensuring", "Solid infill pattern that improves coverage in narrow areas.", Slic3r::ipEnsuring},
}};

bool requires_unmigrated_object_state(Slic3r::InfillPattern pattern)
{
    // These legacy algorithms depend on object-level data prepared by the old
    // PrintObject::prepare_infill() path. They are still registered so presets
    // and the dynamic enum know about them, but the wrapper fails cleanly until
    // the new infill pipeline exposes their precomputed state.
    return pattern == Slic3r::ipAdaptiveCubic ||
           pattern == Slic3r::ipSupportCubic ||
           pattern == Slic3r::ipLightning;
}

Slic3r::Flow flow_from_raw(const c_flow &flow)
{
    // c_flow is the ABI representation prepared by the host step. Flow does
    // not expose a public exact-value constructor, so rebuild it from spacing,
    // height and nozzle diameter. This preserves the centerline spacing that
    // pattern generators primarily use.
    if (flow.spacing > 0 && flow.height > 0 && flow.nozzle_diameter > 0)
        return Slic3r::Flow::new_from_spacing(float(Slic3r::unscaled(flow.spacing)),
                                              float(Slic3r::unscaled(flow.nozzle_diameter)),
                                              float(Slic3r::unscaled(flow.height)),
                                              flow.spacing_ratio,
                                              flow.is_bridge != 0);
    return {};
}

Slic3r::FillParams fill_params_from_raw(const raw_infill_pattern_params &raw,
                                        const Slic3r::PrintRegionConfig &region_config)
{
    Slic3r::FillParams params;
    params.density = raw.density;
    params.bridge_offset = raw.bridge_offset;
    params.flow_mult = raw.flow_mult;
    params.connection = static_cast<Slic3r::InfillConnection>(raw.connection);
    params.add_gap_fill = raw.add_gap_fill != 0;
    params.anchor_length = raw.anchor_length;
    params.anchor_length_max = raw.anchor_length_max;
    params.fill_resolution = raw.fill_resolution;
    params.dont_adjust = raw.dont_adjust != 0;
    params.monotonic = raw.monotonic != 0;
    params.fill_exactly = raw.fill_exactly != 0;
    params.complete = raw.complete != 0;
    params.role = Slic3r::ExtrusionRole(static_cast<Slic3r::ExtrusionRoleModifier>(raw.extrusion_role));
    params.flow = flow_from_raw(raw.flow);
    params.priority = raw.priority;
    params.config = &region_config;
    params.extruder = raw.extruder;
    params.use_arachne = raw.use_arachne != 0;
    params.layer_height = float(raw.layer_height);
    params.max_sparse_infill_spacing = raw.max_sparse_infill_spacing;
    return params;
}

class LegacyInfillPattern : public PluginBase
{
public:
    LegacyInfillPattern(orchestrator_handle *orch,
                        const char *id,
                        const char *name,
                        const char *description,
                        Slic3r::InfillPattern pattern,
                        int32_t priority)
        : PluginBase(orch)
        , m_id(id)
        , m_name(name)
        , m_description(description)
        , m_pattern(pattern)
        , m_priority(priority)
    {}

private:
    const char *id_impl() const noexcept override { return m_id; }
    const char *name_impl() const noexcept override { return m_name; }
    const char *description_impl() const noexcept override { return m_description; }
    slicing_step_t step_impl() const noexcept override { return INFILL_PATTERN; }
    const char *const *dependencies_impl() const noexcept override { return k_no_dependencies; }
    int32_t priority_impl() const noexcept override { return m_priority; }
    const char *progress_message_format_impl() const noexcept override { return "Generating infill pattern: %u / %u"; }

    void run_impl(const plugin_run_context *run_ctx) const override
    {
        const run_ctx_infill_pattern *ctx = plugin_ctx_as_infill_pattern(run_ctx);
        assert(ctx != nullptr);
        if (ctx == nullptr || ctx->surface == nullptr || ctx->params == nullptr || ctx->output == nullptr)
            return;

        const Slic3r::Surface *surface = reinterpret_cast<const Slic3r::Surface *>(ctx->surface);
        Slic3r::ExtrusionEntityCollection *output =
            reinterpret_cast<Slic3r::ExtrusionEntityCollection *>(ctx->output);
        const Slic3r::Print *print = reinterpret_cast<const Slic3r::Print *>(ctx->print);
        const Slic3r::PrintObject *object = reinterpret_cast<const Slic3r::PrintObject *>(ctx->object);
        const Slic3r::LayerRegion *region =
            reinterpret_cast<const Slic3r::LayerRegion *>(ctx->primary_region);
        const Slic3r::ExPolygons *no_overlap =
            reinterpret_cast<const Slic3r::ExPolygons *>(ctx->no_overlap_areas);
        if (surface == nullptr || output == nullptr || print == nullptr || object == nullptr || region == nullptr)
            return;

        if (requires_unmigrated_object_state(m_pattern)) {
            throw std::runtime_error(
                std::string("Legacy infill pattern '") + m_id +
                "' requires object-level precomputed data that is not exposed to INFILL_PATTERN plugins yet.");
        }

        Slic3r::FillParams params = fill_params_from_raw(*ctx->params, region->region().config());
        std::unique_ptr<Slic3r::Fill> filler(Slic3r::Fill::new_from_type(m_pattern));
        filler->set_bounding_box(object->bounding_box());
        filler->layer_id = ctx->params->layer_id;
        filler->z = ctx->params->z;
        filler->angle = float(ctx->params->angle);
        filler->can_angle_cross = float(ctx->params->can_angle_cross != 0);
        filler->link_max_length = ctx->params->link_max_length;
        filler->loop_clipping = ctx->params->loop_clipping;
        filler->overlap = ctx->params->overlap;
        filler->set_config(&print->config(), &object->config());

        if (no_overlap != nullptr)
            filler->no_overlap_expolygons = *no_overlap;

        // The legacy Fill class still owns the low-level line generation for
        // these first migrated patterns. The new plugin boundary is already in
        // place: when the algorithms are rewritten against the pure C ABI, this
        // wrapper can disappear without changing the STEP_INFILL host wrapper.
        filler->init_spacing(ctx->params->spacing, params);
        filler->fill_surface_extrusion(surface, params, *output);
        progress().increment();
    }

    const char *m_id = "";
    const char *m_name = "";
    const char *m_description = "";
    Slic3r::InfillPattern m_pattern = Slic3r::ipRectilinear;
    int32_t m_priority = 0;
};

std::vector<std::unique_ptr<LegacyInfillPattern>> &legacy_instances(orchestrator_handle *orch)
{
    static std::vector<std::unique_ptr<LegacyInfillPattern>> plugins;
    if (plugins.empty()) {
        plugins.reserve(k_legacy_patterns.size());
        for (size_t idx = 0; idx < k_legacy_patterns.size(); ++idx) {
            const LegacyPatternDefinition &def = k_legacy_patterns[idx];
            // Use declaration order as priority. The dynamic enum builder keeps
            // active INFILL_PATTERN plugins sorted by plugin priority, so this
            // preserves the familiar order from the legacy config UI.
            plugins.emplace_back(
                new LegacyInfillPattern(orch, def.id, def.name, def.description, def.pattern, int32_t(idx)));
        }
    }
    return plugins;
}

} // namespace

void register_legacy_infill_pattern_plugins(orchestrator_handle *orch)
{
    for (const std::unique_ptr<LegacyInfillPattern> &plugin : legacy_instances(orch))
        orchestrator_register_plugin(orch, plugin->c_instance());
}

}}} // namespace slic3r_api::Infill::LegacyInfillPatternsPlugin
