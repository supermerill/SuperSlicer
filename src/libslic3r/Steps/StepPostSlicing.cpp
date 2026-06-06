///|/ Copyright (c) SuperSlicer 2026 Durand R?mi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/

#include "StepPostSlicing.hpp"

#include <cmath>
#include <sstream>

#include "libslic3r/Api/host/Orchestrator.hpp"
#include "libslic3r/Api/host/Plugin.hpp"
#include "libslic3r/Api/host/steps/PostSlicingStep.hpp"
#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/ExPolygon.hpp"
#include "libslic3r/Layer.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintObject.hpp"
#include "libslic3r/SurfaceCollection.hpp"
#include "libslic3r/Thread.hpp"

#include "StepPipeline.hpp"
#include "StepRunner.hpp"

namespace Slic3r::Steps::StepPostSlicing {
namespace {

bool has_non_empty_expolygon(const ExPolygons &expolygons)
{
    for (const ExPolygon &expolygon : expolygons)
        if (!expolygon.empty())
            return true;
    return false;
}

bool has_significant_overlap(const ExPolygons &lhs, const ExPolygons &rhs, double *overlap_area)
{
    if (lhs.empty() || rhs.empty()) {
        if (overlap_area != nullptr)
            *overlap_area = 0.;
        return false;
    }

    const ExPolygons overlap = intersection_ex(lhs, rhs);
    const double area_overlap = std::abs(area(overlap));
    if (overlap_area != nullptr)
        *overlap_area = area_overlap;

    // Adjacent regions may produce microscopic Clipper slivers along a shared
    // border. Anything larger than this is a real layer-region overlap.
    const double max_tolerated_overlap_area =
        double(SCALED_EPSILON) * double(SCALED_EPSILON) * 10.;
    return area_overlap > max_tolerated_overlap_area;
}

bool validate_layers(const Print &print, std::string &out_error, const bool require_non_empty_region_slices)
{
    bool ok = true;

    for (size_t object_idx = 0; object_idx < print.objects().size(); ++object_idx) {
        const PrintObject &object = print.objects()[object_idx];

        for (size_t layer_idx = 0; layer_idx < object.layer_count(); ++layer_idx) {
            const Layer &layer = object.layer(layer_idx);

            std::ostringstream layer_prefix;
            layer_prefix << "object " << object_idx << ", layer " << layer_idx << ": ";

            if (!has_non_empty_expolygon(layer.lslices())) {
                ok = false;
                out_error += layer_prefix.str() + "layer slices do not contain any non-empty ExPolygon";
            }

            if (layer.islands().size() != layer.lslices().size()) {
                ok = false;
                std::ostringstream msg;
                msg << layer_prefix.str() << "island count (" << layer.islands().size()
                    << ") does not match layer slice count (" << layer.lslices().size() << ")";
                out_error += msg.str();
            }

            for (size_t island_idx = 0; island_idx < layer.islands().size(); ++island_idx) {
                const LayerSliceIsland &island = layer.islands()[island_idx];

                if (island.get_slice().empty()) {
                    ok = false;
                    std::ostringstream msg;
                    msg << layer_prefix.str() << "island " << island_idx << " has an empty slice";
                    out_error += msg.str();
                }

                if (!island.regions_islands().empty()) {
                    ok = false;
                    std::ostringstream msg;
                    msg << layer_prefix.str() << "island " << island_idx
                        << " already has LayerRegionIsland data";
                    out_error += msg.str();
                }
            }

            for (size_t region_idx = 0; region_idx < layer.region_count(); ++region_idx) {
                const LayerRegion &region = layer.region(region_idx);

                std::ostringstream region_prefix;
                region_prefix << layer_prefix.str() << "region " << region_idx << ": ";

                if (require_non_empty_region_slices && !has_non_empty_expolygon(region.get_raw_slices())) {
                    ok = false;
                    out_error += region_prefix.str() + "raw slices do not contain any non-empty ExPolygon";
                }

                if (!region.slices().empty()) {
                    ok = false;
                    out_error += region_prefix.str() + "processed surfaces are not empty";
                }

                if (!region.fill_surfaces().empty()) {
                    ok = false;
                    out_error += region_prefix.str() + "fill surfaces are not empty";
                }
            }

            for (size_t region_idx = 0; region_idx < layer.region_count(); ++region_idx) {
                const LayerRegion &region = layer.region(region_idx);
                for (size_t other_region_idx = region_idx + 1; other_region_idx < layer.region_count(); ++other_region_idx) {
                    const LayerRegion &other_region = layer.region(other_region_idx);
                    double overlap_area = 0.;
                    if (has_significant_overlap(region.get_raw_slices(), other_region.get_raw_slices(), &overlap_area)) {
                        ok = false;
                        std::ostringstream msg;
                        msg << layer_prefix.str() << "regions " << region_idx << " and " << other_region_idx
                            << " have overlapping raw slices, overlap scaled area " << overlap_area;
                        out_error += msg.str();
                    }
                }
            }
        }
    }

    return ok;
}

} // namespace

void clean_and_prepare(Print &) {}

bool validate_pre(const Print &print, std::string &out_error)
{
    // A PrintObject can expose PrintRegions that are not used on every layer.
    // The validator therefore checks the merged layer geometry and inter-region
    // overlaps, but it does not require every possible LayerRegion to contain
    // raw polygons.
    return validate_layers(print, out_error, false);
}

bool validate_post(const Print &print, std::string &error)
{
    // Post-slicing plugins may remove a whole region from a layer while keeping
    // the layer itself valid. For example, a vase-mode cleanup plugin can keep
    // only one disconnected component and leave the dropped component's region
    // empty. Later steps rebuild LayerRegionIsland membership from the final
    // raw slices, so this is valid after the plugins have run.
    return validate_layers(print, error, false);
}

namespace {

void attach_regions_to_islands(Print &print)
{
    // Slicing and post-slicing plugins own the raw LayerRegion slices and may
    // rebuild the geometric LayerSliceIsland list. The perimeter and surface
    // steps then need each island to know which LayerRegions intersect it, so
    // this finalizes that relation once all post-slicing geometry edits are
    // finished.
    for (PrintObject &object : print.objects())
        for (Layer &layer : object.layers())
            layer.add_regions_to_islands();
}

void rebuild_island_overlap_graph(Print &print)
{
    // Surface classification and several perimeter modules ask an island for
    // its direct upper/lower neighbors. Those links are not implied by the raw
    // slice polygons; they must be rebuilt after the final post-slicing edit
    // has finished changing island geometry.
    for (PrintObject &object : print.objects()) {
        for (Layer &layer : object.layers()) {
            for (LayerSliceIsland &island : layer.islands()) {
                island.overlaps_above.clear();
                island.overlaps_below.clear();
            }
        }

        for (size_t layer_idx = 1; layer_idx < object.layer_count(); ++layer_idx)
            Layer::build_up_down_graph(object.layer(layer_idx - 1), object.layer(layer_idx));
    }
}

} // namespace

void run_step(Orchestrator &orchestrator, Print &print)
{
    Detail::validate_or_report(validate_pre, print, "Post-slicing pre-step validation");

    std::vector<Plugin *> plugins = selected_or_active_plugins_for_step(orchestrator,
                                                                        STEP_POST_SLICING,
                                                                        &print.full_print_config());

    for (Plugin *plugin : plugins) {
        const size_t run_count = print.objects().size();
        plugin_host_context host_context = orchestrator.prepare_plugin_host_context(STEP_POST_SLICING, plugin, &print);
        plugin_run_context run_context = orchestrator.prepare_plugin_run_context(STEP_POST_SLICING,
                                                                                plugin,
                                                                                &host_context);
        plugin->setup(run_context, uint32_t(run_count));

        // Each worker gets its own plugin_run_context copy and step payload. The
        // shared run_context only carries stable host/plugin callbacks.
        parallel_for(size_t(0), run_count, [&print, plugin, &run_context](const size_t idx) {
            plugin_run_context context_copy = run_context;
            if (context_copy.is_cancelled != nullptr && context_copy.is_cancelled(context_copy.host_context))
                return;

            run_ctx_post_slicing context_step = ApiHost::Steps::make_post_slicing_run_context(print, idx);
            context_copy.data = &context_step;
            plugin->setup_run(context_copy);
        });

        parallel_for(size_t(0), run_count, [&print, plugin, &run_context](const size_t idx) {
            plugin_run_context context_copy = run_context;
            if (context_copy.is_cancelled != nullptr && context_copy.is_cancelled(context_copy.host_context))
                return;

            run_ctx_post_slicing context_step = ApiHost::Steps::make_post_slicing_run_context(print, idx);
            context_copy.data = &context_step;
            plugin->run(context_copy);
        });

        Detail::validate_or_report(validate_post, print, "Post-slicing post-plugin validation");
    }

    attach_regions_to_islands(print);
    rebuild_island_overlap_graph(print);

    //old post-clicing, replaced by plugins
//    this->_max_overhang_threshold();
}

} // namespace Slic3r::Steps::StepPostSlicing
