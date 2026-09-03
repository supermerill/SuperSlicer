///|/ Copyright (c) SuperSlicer 2026 Durand Rémi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/

#include "OnlyOnePerimeterOnTop.hpp"

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

#include "libslic3r/Api/plugin/c/slic3r_orchestrator.h"
#include "libslic3r/Api/plugin/c/steps/slic3r_step_perimeter.h"
#include "libslic3r/Api/plugin/cpp/ClipperViews.hpp"
#include "libslic3r/Api/plugin/cpp/PerimeterStepViews.hpp"

/*
Top-surface single-perimeter module
===================================

This plugin registers a PERIMETER_GENERATION_MODULE that limits enabled top
surface areas to one perimeter. It does not generate paths. The perimeter
generator first creates the current node and its children; this module then
classifies the child domain and changes the requested count of only the parts
that should stop after the first perimeter.

module_start() stores the current island's region-segregated settings and
whether upper islands exist. If there is no upper island and the setting is
uniformly enabled, the root can be clamped immediately. All other cases wait
for module_after(), because the child domain and the first generated perimeter
are needed to classify the top area.

For a real top layer, enabled region clips are applied directly to the child
nodes. For a partial top surface, the module builds lower and upper coverage,
accounts for bridging, external-infill margins, and minimum top width, then
partitions the child domain into a stop area and a normal continuation area.
The stop area is split out of the children and clamped to one perimeter; the
remaining area keeps the normal perimeter requests. Region entries are
processed successively so their different top-surface parameters do not
classify an already-processed stop area twice.

The normal call flow is:

    OnlyOnePerimeterOnTop::run_impl()
    `-- install module_vtable() in the perimeter-generation context
        `-- perimeter generator invokes module_start()
            |-- segregate top and top-fill settings
            |-- no upper island and uniform enabled setting?
            |   `-- clamp the root to one perimeter
            `-- otherwise defer classification to module_after()

    module_after()
    `-- accept only the first generated perimeter with children
        |-- no upper island?
        |   `-- split enabled regions and clamp their child nodes
        `-- upper island exists?
            |-- child_area_collection()
            |-- build_one_perimeter_stop_area() for each enabled region entry
            |   |-- build upper/lower coverage and bridge checker
            |   |-- classify top candidate and normal continuation
            |   `-- accumulate the exact stop-area partition
            `-- set_top_children_to_one_perimeter()
                `-- split children and clamp the stop pieces

module_end() releases the tree-local state. The module changes only perimeter
requests and node subdivision; the selected perimeter generator remains
responsible for producing the extrusion paths.
*/

namespace slic3r_api { namespace Perimeter { namespace OnlyOnePerimeterOnTopPlugin {

namespace {

const char *k_only_one_perimeter_on_top_id = "perimeter.module.only_one_perimeter_on_top";
const char *k_no_dependencies[] = { nullptr };
const raw_used_config_key k_used_config_keys[] = {
    { "only_one_perimeter_top", RAW_CO_BOOL, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE },
    { "min_width_top_surface", RAW_CO_FLOAT_OR_PERCENT, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE },
    { "only_one_perimeter_top_other_algo", RAW_CO_BOOL, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE },
    { "external_infill_margin", RAW_CO_FLOAT_OR_PERCENT, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE },
    { "bridged_infill_margin", RAW_CO_FLOAT_OR_PERCENT, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE }
};
const char *k_only_one_perimeter_top_key = "only_one_perimeter_top";
const char *k_min_width_top_surface_key = "min_width_top_surface";
const char *k_only_one_perimeter_top_other_algo_key = "only_one_perimeter_top_other_algo";
const char *k_external_infill_margin_key = "external_infill_margin";
const char *k_bridged_infill_margin_key = "bridged_infill_margin";

class ModuleState
{
public:
    void initialize_region_settings(const PerimeterGenerationContextView &context)
    {
        // The island, its top/bottom neighborhood and its region settings are
        // invariant while the host walks this perimeter tree. Build the
        // expensive RegionSettings maps once in start(); after() only reads
        // them for each generated node.
        const LayerIsland island = context.island();
        m_region_count = island.region_count();
        m_has_upper_islands = island.upper_island_count() > 0;

        m_top_settings.reset(new RegionSettings(context.storage(), island, {{k_only_one_perimeter_top_key}}));
        m_top_settings->segregate(island.slice());

        m_top_fill_settings.reset(new RegionSettings(context.storage(), island,
            {{k_only_one_perimeter_top_key, k_min_width_top_surface_key, k_only_one_perimeter_top_other_algo_key}}));
        m_top_fill_settings->segregate(island.slice());
    }

    bool ready() const
    {
        return m_region_count > 0 && m_top_settings != nullptr && m_top_fill_settings != nullptr;
    }

    uint32_t region_count() const
    {
        return m_region_count;
    }

    bool has_upper_islands() const
    {
        return m_has_upper_islands;
    }

    const RegionSettings *top_settings() const
    {
        return m_top_settings.get();
    }

    const RegionSettings *top_fill_settings() const
    {
        return m_top_fill_settings.get();
    }

private:
    uint32_t m_region_count = 0;
    bool m_has_upper_islands = false;
    std::unique_ptr<RegionSettings> m_top_settings;
    std::unique_ptr<RegionSettings> m_top_fill_settings;
};

// This module currently uses the first region as the source of shared print
// settings. Region-local variations are handled separately through
// RegionSettings clips.
Config region_config(const PerimeterGenerationContextView &context)
{
    return context.island().region(0).print_region().config();
}

// The top-surface computations are expressed relative to the external wall:
// its width reserves the first perimeter, and its spacing defines the safe fill
// area just inside that wall.
c_flow external_perimeter_flow(const PerimeterGenerationContextView &context)
{
    return context.island().region(0).flow(RAW_EXTRUSION_ROLE_EXTERNAL_PERIMETER);
}

// Wrap a single ExPolygon view into a temporary collection so it can be passed
// to the collection-based Clipper helpers without special one-off overloads.
StoredExPolygonCollection collection_from_expolygon(storage_handle *storage, const ExPolygon &expolygon)
{
    StoredExPolygonCollection collection(storage);
    collection.push_back(expolygon);
    return collection;
}

// Promote a flat Polygon into an ExPolygon with no holes. Some contour-growth
// operations naturally produce raw polygons; perimeter modules exchange areas
// as ExPolygons, so normalize them at the boundary.
StoredExPolygonCollection collection_from_polygon(storage_handle *storage, const Polygon &polygon)
{
    StoredExPolygon expolygon(storage);
    multipoint_copy(polygon_as_multipoint(expolygon_contour(expolygon.mutable_handle())), polygon.multipoint_handle());

    StoredExPolygonCollection collection(storage);
    collection.push_back(expolygon.readonly());
    return collection;
}

// Build the union of lower islands that may support the current island. The
// host already filters this neighborhood, so this is intentionally local to the
// current LayerIsland instead of scanning the whole layer.
StoredExPolygonCollection lower_slice_coverage(storage_handle *storage, const LayerIsland &island)
{
    StoredExPolygonCollection lower_slices(storage);
    const std::vector<LayerIsland> lower_islands = island.lower_islands();
    for (const LayerIsland &lower_island : lower_islands)
        lower_slices.push_back(lower_island.slice());

    if (lower_slices.empty())
        return lower_slices;

    ClipperContext clipper(storage);
    return clipper_union(clipper(lower_slices)).to_expolygon_collection();
}

// Build the union of upper islands. This coverage is later grown to decide
// which parts of the current perimeter branch are not real top surfaces.
StoredExPolygonCollection upper_slice_coverage(storage_handle *storage, const LayerIsland &island)
{
    StoredExPolygonCollection upper_slices(storage);
    const std::vector<LayerIsland> upper_islands = island.upper_islands();
    for (const LayerIsland &upper_island : upper_islands)
        upper_slices.push_back(upper_island.slice());

    if (upper_slices.empty())
        return upper_slices;

    ClipperContext clipper(storage);
    return clipper_union(clipper(upper_slices)).to_expolygon_collection();
}

// Append two temporary collections and normalize them with a union. This keeps
// later diffs/intersections simpler: callers can treat the result as one
// coherent area even if pieces came from different RegionSettings entries.
StoredExPolygonCollection union_append(storage_handle *storage,
                                       StoredExPolygonCollection &&lhs,
                                       StoredExPolygonCollection &&rhs)
{
    if (!rhs.empty())
        lhs.append_move_from(std::move(rhs));
    if (lhs.empty())
        return std::move(lhs);

    ClipperContext clipper(storage);
    return clipper_union(clipper(lhs)).to_expolygon_collection();
}

// Common one-line geometry helpers. They centralize the storage/ClipperContext
// ceremony so the algorithm below reads in terms of areas rather than handles.
StoredExPolygonCollection offset_collection(storage_handle *storage,
                                            const ExPolygonCollection &subject,
                                            double delta)
{
    ClipperContext clipper(storage);
    return clipper_offset(clipper(subject), delta).to_expolygon_collection();
}

StoredExPolygonCollection diff_collection(storage_handle *storage,
                                          const ExPolygonCollection &subject,
                                          const ExPolygonCollection &clip_area)
{
    if (subject.empty() || clip_area.empty())
        return subject.clone(storage);

    ClipperContext clipper(storage);
    return clipper_diff_with_safety_offset(clipper(subject), clipper(clip_area)).to_expolygon_collection();
}

StoredExPolygonCollection intersection_collection(storage_handle *storage,
                                                  const ExPolygonCollection &subject,
                                                  const ExPolygonCollection &clip_area)
{
    if (subject.empty() || clip_area.empty())
        return StoredExPolygonCollection(storage);

    ClipperContext clipper(storage);
    return clipper_intersection_with_safety_offset(clipper(subject), clipper(clip_area)).to_expolygon_collection();
}

// Return the bounding box of an ExPolygon collection using contours only. Holes
// are inside their contour, so they cannot enlarge the collection bounds.
bool collection_bounding_box(const ExPolygonCollection &collection, c_bounding_box &out)
{
    bool initialized = false;
    for (const ExPolygon &expolygon : collection) {
        const Polygon contour = expolygon.contour();
        if (contour.empty())
            continue;

        const c_bounding_box bbox = contour.bounding_box();
        if (!initialized) {
            out = bbox;
            initialized = true;
            continue;
        }

        out.min.x = std::min(out.min.x, bbox.min.x);
        out.min.y = std::min(out.min.y, bbox.min.y);
        out.max.x = std::max(out.max.x, bbox.max.x);
        out.max.y = std::max(out.max.y, bbox.max.y);
    }
    return initialized;
}

// Inflate an ABI bounding box by a scaled slicer-space margin.
c_bounding_box inflated_bounding_box(c_bounding_box bbox, coord_t delta)
{
    bbox.min.x -= delta;
    bbox.min.y -= delta;
    bbox.max.x += delta;
    bbox.max.y += delta;
    return bbox;
}

// Clip the possible clip area to the subject bbox before expensive boolean
// operations. The perimeter pipeline already works on one island and nearby
// upper/lower islands, but this cheap pre-pass still keeps worst-case path
// counts bounded when the neighborhood contains many small pieces.
StoredExPolygonCollection clip_to_subject_bbox(storage_handle *storage,
                                               const ExPolygonCollection &src,
                                               const ExPolygonCollection &subject)
{
    if (src.empty())
        return StoredExPolygonCollection(storage);

    c_bounding_box subject_bbox = {};
    if (!collection_bounding_box(subject, subject_bbox))
        return StoredExPolygonCollection(storage);

    return StoredExPolygonCollection::adopt(storage,
        clipper_clip_expolygons_with_subject_bbox(storage, src.handle(),
            inflated_bounding_box(subject_bbox, SCALED_EPSILON)));
}

// Return the upper coverage used by this RegionSettings entry. When the
// setting is region-local, areas outside the enabled clip are deliberately
// added to the upper coverage so they cannot be classified as top fill.
StoredExPolygonCollection build_upper_slices_for_area(const PerimeterGenerationContextView &context,
                                                      const RegionSettingsClip &enabled_area,
                                                      const ExPolygonCollection &domain)
{
    storage_handle *storage = context.storage();
    StoredExPolygonCollection upper_slices = upper_slice_coverage(storage, context.island());
    if (enabled_area.is_accept_all())
        return upper_slices;

    // Outside the setting-enabled area, behave as if an upper layer existed.
    // This prevents the module from forcing one perimeter where the setting is
    // disabled by a region/modifier split.
    StoredExPolygonCollection disabled_area = enabled_area.diff(domain);
    return union_append(storage, std::move(upper_slices), std::move(disabled_area));
}

// Approximate the current area that should be considered bridging. Start with
// current area minus lower coverage, then grow/clean it by the configured
// bridge margin so small supported islands near the edge do not incorrectly
// mark the area as ordinary top fill.
StoredExPolygonCollection build_bridge_checker(const PerimeterGenerationContextView &context,
                                               const Config &config,
                                               const ExPolygonCollection &orig_polygons,
                                               const ExPolygonCollection &lower_slices,
                                               uint32_t perimeter_count)
{
    storage_handle *storage = context.storage();
    if (lower_slices.empty())
        return StoredExPolygonCollection(storage);

    const c_flow perimeter_flow = context.perimeter_flow();
    const c_flow ext_flow = external_perimeter_flow(context);
    const double bridge_margin = config.get(k_bridged_infill_margin_key).get_effective_value(unscaled(ext_flow.width));
    double bridge_offset = double(perimeter_flow.spacing) * double(perimeter_count) + scale_d(bridge_margin);
    StoredExPolygonCollection lower_slices_clipped = clip_to_subject_bbox(storage, lower_slices, orig_polygons);
    // If the clipped lower layer is empty, diff_collection() intentionally
    // returns orig_polygons: this area has no lower support under the subject
    // bbox and is therefore fully bridge/overhang candidate.
    StoredExPolygonCollection bridge_checker = diff_collection(storage, orig_polygons, lower_slices_clipped);

    while (bridge_offset > SCALED_EPSILON && !bridge_checker.empty()) {
        double current_offset = double(perimeter_flow.spacing);
        if (bridge_offset < double(perimeter_flow.spacing) * 1.5)
            current_offset = bridge_offset;
        bridge_offset -= current_offset;

        StoredExPolygonCollection grown = offset_collection(storage, bridge_checker, current_offset);
        StoredExPolygonCollection clipped = intersection_collection(storage, grown, orig_polygons);
        ClipperContext clipper(storage);
        bridge_checker = clipper_offset2(clipper(clipped), -current_offset, current_offset).to_expolygon_collection();
    }

    return bridge_checker;
}

StoredExPolygonCollection grow_upper_slices_preserving_islands(storage_handle *storage,
                                                               const ExPolygonCollection &upper_slices,
                                                               coord_t offset_top_surface,
                                                               coordf_t min_width_top_surface)
{
    // A single offset2() on the whole upper-slice collection is not equivalent:
    // it may merge nearby islands during the shrink/grow pass, and it lets hole
    // topology participate in the contour offset. Processing each ExPolygon
    // independently keeps thin upper islands disappearing locally without
    // turning close-but-separate islands into one blocking blob.
    ClipperContext clipper(storage);
    StoredExPolygonCollection grown_accumulator(storage);

    for (const ExPolygon &expolygon : upper_slices) {
        StoredExPolygon contour_expolygon(storage);
        multipoint_copy(polygon_as_multipoint(expolygon_contour(contour_expolygon.mutable_handle())),
                        expolygon.contour().multipoint_handle());

        StoredPolygonCollection grown_contours =
            clipper_offset2(clipper(contour_expolygon.readonly()), -double(offset_top_surface),
                            double(offset_top_surface) + double(min_width_top_surface)).to_polygon_collection();
        if (grown_contours.empty())
            continue;

        if (expolygon.hole_size() == 0) {
            for (const Polygon &contour : grown_contours)
                grown_accumulator.append_move_from(collection_from_polygon(storage, contour));
            continue;
        }

        StoredPolygonCollection holes(storage);
        for (uint32_t hole_idx = 0; hole_idx < expolygon.hole_size(); ++hole_idx) {
            StoredPolygon hole(storage);
            hole.copy_from(expolygon.hole(hole_idx));
            hole.reverse();
            holes.push_back(hole.readonly());
        }

        StoredPolygonCollection shrunken_holes =
            clipper_offset(clipper(holes.readonly()), -double(min_width_top_surface)).to_polygon_collection();
        StoredExPolygonCollection grown_with_holes =
            clipper_diff(clipper(grown_contours.readonly()), clipper(shrunken_holes.readonly())).to_expolygon_collection();
        grown_accumulator.append_move_from(std::move(grown_with_holes));
    }

    if (grown_accumulator.empty())
        return grown_accumulator;
    return clipper_union(clipper(grown_accumulator)).to_expolygon_collection();
}

// Build the geometric domain owned by the children created from the first
// perimeter. These children are where the next perimeter would be generated.
// The module classifies parts of this domain as "continue" or "stop here".
StoredExPolygonCollection child_area_collection(storage_handle *storage, const PerimeterNodeView &parent)
{
    StoredExPolygonCollection children_area(storage);
    const std::vector<PerimeterNodeView> children = parent.children_snapshot();
    for (const PerimeterNodeView &child : children)
        children_area.push_back(child.area());

    if (children_area.empty())
        return children_area;

    ClipperContext clipper(storage);
    return clipper_union(clipper(children_area)).to_expolygon_collection();
}

// Find the part of source_child_area that should stop after the first perimeter.
//
// source_child_area is expressed in child-node coordinates: it is the space
// where the second perimeter would normally be generated. Returning a subset of
// that space lets the caller split existing child nodes directly, without
// reconstructing a perimeter centerline or touching the extrusion that was just
// emitted.
//
// normal_child_area is the remaining child domain passed between RegionSettings
// entries. When several region configurations are active, each entry replaces
// it with the area that still needs ordinary inner perimeter generation.
StoredExPolygonCollection build_one_perimeter_stop_area(const PerimeterGenerationContextView &context,
                                                        const PerimeterNodeView &parent,
                                                        const RegionSettingsValue &values,
                                                        const RegionSettingsClip &enabled_area,
                                                        const ExPolygonCollection &source_child_area,
                                                        StoredExPolygonCollection &normal_child_area)
{
    storage_handle *storage = context.storage();
    if (source_child_area.empty())
        return StoredExPolygonCollection(storage);

    Config config = region_config(context);
    const c_flow perimeter_flow = context.perimeter_flow();
    const c_flow ext_flow = external_perimeter_flow(context);
    const uint32_t inner_perimeter_count = parent.perimeter_needed() > 0 ? parent.perimeter_needed() - 1 : 0;

    // external_infill_margin anchors the top fill under the perimeter stack.
    // Part of that distance is already consumed by the inner perimeters that
    // still exist, so keep only the excess margin that really has to push the
    // one-perimeter top area inward.
    const double max_perimeters_width = unscaled(double(ext_flow.width) + double(perimeter_flow.spacing) * double(inner_perimeter_count));
    coord_t offset_top_surface =
        scale_i(config.get(k_external_infill_margin_key).get_effective_value(inner_perimeter_count == 0 ? 0. : max_perimeters_width));
    if (offset_top_surface > 0.9 * (inner_perimeter_count <= 1 ? 0. : double(perimeter_flow.spacing) * double(inner_perimeter_count - 1)))
        offset_top_surface -= coord_t(0.9 * (inner_perimeter_count <= 1 ? 0. : double(perimeter_flow.spacing) * double(inner_perimeter_count - 1)));
    else
        offset_top_surface = 0;

    const double configured_min_width =
        values.get_effective_value(unscaled(perimeter_flow.width), k_min_width_top_surface_key);
    const coordf_t min_width_top_surface =
        std::max(coordf_t(double(ext_flow.spacing) / 2.0 + 10.0), scale_d(configured_min_width));

    // The upper layer marks areas that are not true top surfaces. Region clips
    // are folded into build_upper_slices_for_area(): outside the enabled region
    // is treated as covered by an upper layer, so only enabled child areas can
    // become one-perimeter top fill.
    StoredExPolygonCollection upper_slices =
        build_upper_slices_for_area(context, enabled_area, source_child_area);
    ClipperContext clipper(storage);
    StoredExPolygonCollection grown_upper_slices(storage);
    if (upper_slices.empty()) {
        grown_upper_slices = StoredExPolygonCollection(storage);
    } else if (!values.get_bool(k_only_one_perimeter_top_other_algo_key)) {
        grown_upper_slices = clipper_offset(clipper(upper_slices), min_width_top_surface).to_expolygon_collection();
    } else {
        grown_upper_slices =
            grow_upper_slices_preserving_islands(storage, upper_slices, offset_top_surface, min_width_top_surface);
    }
    grown_upper_slices = clip_to_subject_bbox(storage, grown_upper_slices, source_child_area);

    // Bridged zones should not be treated as ordinary top fill. They need their
    // own bridging behavior, so detect unsupported regions from the lower layer
    // and remove them from the top-surface candidate before classifying it.
    StoredExPolygonCollection source_without_bridge = source_child_area.clone(storage);
    StoredExPolygonCollection lower_slices = lower_slice_coverage(storage, context.island());
    StoredExPolygonCollection bridge_checker =
        build_bridge_checker(context, config, source_child_area, lower_slices, inner_perimeter_count);
    if (!bridge_checker.empty())
        source_without_bridge = diff_collection(storage, source_child_area, bridge_checker);

    // Candidate top area: child space that is enabled, not bridging, and not
    // protected by the upper layer after the configured minimum-width growth.
    StoredExPolygonCollection top_candidate =
        diff_collection(storage, source_without_bridge, grown_upper_slices);

    // Grow the candidate by the configured anchor/min-width distance. The
    // complement inside source_child_area is the area that still needs normal
    // inner perimeter generation.
    StoredExPolygonCollection top_offset =
        offset_collection(storage, top_candidate,
                          double(offset_top_surface) + min_width_top_surface - double(ext_flow.spacing) / 2.0);
    StoredExPolygonCollection continue_area =
        diff_collection(storage, source_child_area, top_offset);

    // Return an exact partition of the child domain. This is deliberately based
    // on source_child_area rather than on an offset centerline: no geometry is
    // lost between the "stop" and "continue" branches, and gap-fill creation is
    // left to the later perimeter post-processing step.
    StoredExPolygonCollection stop_area =
        diff_collection(storage, source_child_area, continue_area);

    StoredExPolygonCollection new_normal_child_area =
        intersection_collection(storage, continue_area, source_child_area);
    normal_child_area = std::move(new_normal_child_area);

    return stop_area;
}

// Clamp a whole generated branch to a single perimeter. This is used for the
// easy case where no upper island exists and the enabled top area is the whole
// current node.
void set_children_to_one_perimeter(const PerimeterNodeView &parent)
{
    // The current node already owns the perimeter that has just been generated.
    // Setting the parent and every child to one perimeter stops every branch
    // after that first ring.
    parent.set_perimeter_needed(1);
    const std::vector<PerimeterNodeView> children = parent.children_snapshot();
    for (const PerimeterNodeView &child : children)
        child.set_perimeter_needed(1);
}

// Split a leaf node by a clipping area and return the newly created inside
// nodes. split_node() is only valid on leaf nodes; if the node already has
// children, its geometry has already been branched and must be handled by
// iterating those children instead.
std::vector<PerimeterNodeView> split_node_with_expolygons(perimeter_generation_context *context,
                                                          const PerimeterNodeView &node,
                                                          const ExPolygonCollection &clip)
{
    std::vector<PerimeterNodeView> inside_nodes;
    assert(context != nullptr);
    assert(context == nullptr || context->split_node != nullptr);
    assert(node.valid());
    if (context == nullptr || context->split_node == nullptr || node.child_count() > 0)
        return inside_nodes;

    perimeter_node_span span = {};
    context->split_node(context, node.mutable_handle(), clip.handle(), &span);
    inside_nodes.reserve(span.count);
    for (uint32_t idx = 0; idx < span.count; ++idx)
        if (span.items != nullptr && span.items[idx] != nullptr)
            inside_nodes.push_back(PerimeterNodeView(span.items[idx]));
    return inside_nodes;
}

// For partial top layers, stop_areas is known after the first perimeter and is
// expressed in the child-node coordinate space. Split each child by that area,
// then stop only the pieces that are actually top fill.
void set_top_children_to_one_perimeter(perimeter_generation_context *context,
                                       const PerimeterNodeView &parent,
                                       const ExPolygonCollection &stop_areas)
{
    // Stop areas are only known after the first perimeter was generated. Split
    // each child with those areas, then clamp only the inside pieces.
    const std::vector<PerimeterNodeView> children = parent.children_snapshot();
    for (const PerimeterNodeView &child : children) {
        const std::vector<PerimeterNodeView> inside_nodes = split_node_with_expolygons(context, child, stop_areas);
        if (inside_nodes.empty())
            continue;
        child.set_perimeter_needed(1);
        for (const PerimeterNodeView &inside_node : inside_nodes)
            inside_node.set_perimeter_needed(1);
    }
}

// Same clamping operation as above, but for the no-upper-island case. There is
// no need to reconstruct top-fill geometry: every enabled region area is top.
void set_enabled_children_to_one_perimeter(perimeter_generation_context *context,
                                           const PerimeterNodeView &parent,
                                           const RegionSettingsClip &enabled_area)
{
    if (enabled_area.is_accept_all()) {
        set_children_to_one_perimeter(parent);
        return;
    }

    // Region-local path for real top layers: clamp only the children that fall
    // inside the setting-enabled region. split_node() keeps outside siblings
    // available for normal perimeter generation.
    const std::vector<PerimeterNodeView> children = parent.children_snapshot();
    for (const PerimeterNodeView &child : children) {
        const std::vector<PerimeterNodeView> inside_nodes =
            split_node_with_expolygons(context, child, enabled_area.expolygons());
        for (const PerimeterNodeView &inside_node : inside_nodes)
            inside_node.set_perimeter_needed(1);
    }
}

void *module_start(void *, perimeter_generation_context *context)
{
    // One state object per perimeter tree. Do not store this on the plugin
    // singleton: several islands may be processed in parallel.
    ModuleState *state = new ModuleState();
    assert(context != nullptr);
    assert(context == nullptr || context->root != nullptr);
    if (context == nullptr || context->root == nullptr)
        return state;

    PerimeterGenerationContextView context_view(context);
    const uint32_t region_count = context_view.island().region_count();
    assert(region_count > 0);
    if (region_count == 0)
        return state;

    state->initialize_region_settings(context_view);

    // If there is no upper island at all and the setting is uniform, we can
    // clamp the root immediately. Region-local top settings still need after(),
    // because they must split the children produced by the first perimeter.
    const RegionSettings *settings = state->top_settings();
    assert(settings != nullptr);
    if (!state->has_upper_islands() &&
        !settings->has_many_config(k_only_one_perimeter_top_key) &&
        settings->get_solo_config(k_only_one_perimeter_top_key).get_bool(k_only_one_perimeter_top_key))
        context_view.root().set_perimeter_needed(1);
    return state;
}

void module_after(void *, void *user_context, perimeter_generation_context *context, perimeter_node *node)
{
    // This module acts in after(), because it needs the first generated
    // perimeter to create child areas before it can decide which branches are
    // top surfaces and should stop.
    ModuleState *state = static_cast<ModuleState *>(user_context);
    assert(context != nullptr);
    assert(node != nullptr);
    assert(state != nullptr);
    if (context == nullptr || node == nullptr || state == nullptr)
        return;

    PerimeterGenerationContextView context_view(context);
    const uint32_t region_count = context_view.island().region_count();
    assert(region_count > 0);
    assert(!state->ready() || state->region_count() == region_count);
    if (region_count == 0 || !state->ready())
        return;

    assert(state->has_upper_islands() == (context_view.island().upper_island_count() > 0));

    PerimeterNodeView parent(node);
    // Only the first generated perimeter may clamp its children. Deeper nodes
    // are already post-first-ring branches.
    if (parent.perimeter_idx() > 0 || parent.extrusions().empty() ||
        parent.perimeter_needed() == 0 || parent.child_count() == 0)
        return;

    const RegionSettings *settings = state->has_upper_islands() ? state->top_fill_settings() : state->top_settings();
    assert(settings != nullptr);
    if (settings == nullptr)
        return;

    if (!settings->has_many_config(k_only_one_perimeter_top_key) &&
        !settings->get_solo_config(k_only_one_perimeter_top_key).get_bool(k_only_one_perimeter_top_key))
        return;

    const RegionSettings::AreaMap &areas = settings->get_areas(k_only_one_perimeter_top_key);

    if (!state->has_upper_islands()) {
        // == real top layer code path ==
        // No upper island exists, so every enabled area is a top surface.
        for (const auto &[setting_value, setting_clip] : areas) {
            if (!setting_value.get_bool(k_only_one_perimeter_top_key))
                continue;
            set_enabled_children_to_one_perimeter(context, parent, setting_clip);
        }
        return;
    }

    // == partial top-surface code path ==
    // Upper geometry exists. Work on the children that the first perimeter just
    // created: each child area is the domain where the next perimeter would be
    // generated. The module partitions that domain into:
    // - stop areas, which are top fill and should keep only the first perimeter;
    // - normal areas, which remain available for deeper perimeter generation.
    StoredExPolygonCollection child_domain = child_area_collection(context_view.storage(), parent);
    if (child_domain.empty())
        return;

    StoredExPolygonCollection stop_areas(context_view.storage());
    StoredExPolygonCollection normal_child_area(context_view.storage());
    bool has_processed_enabled_area = false;
    for (const auto &[setting_value, setting_clip] : areas) {
        if (!setting_value.get_bool(k_only_one_perimeter_top_key))
            continue;
        if (has_processed_enabled_area && normal_child_area.empty())
            break;

        // Each enabled RegionSettings entry may have different values for
        // min_width_top_surface / only_one_perimeter_top_other_algo. Process
        // them one after another:
        // - first pass starts from the full child domain;
        // - later passes start from the current normal remainder, so a
        //   stop area found by a previous pass is not classified again.
        StoredExPolygonCollection source_child_area =
            !has_processed_enabled_area ?
            child_domain.readonly().clone(context_view.storage()) :
            normal_child_area.readonly().clone(context_view.storage());

        StoredExPolygonCollection current_stop_area =
            build_one_perimeter_stop_area(context_view, parent, setting_value, setting_clip,
                                          source_child_area, normal_child_area);
        stop_areas = union_append(context_view.storage(), std::move(stop_areas), std::move(current_stop_area));
        has_processed_enabled_area = true;
    }

    if (!stop_areas.empty())
        set_top_children_to_one_perimeter(context, parent, stop_areas);
}

void module_end(void *, void *user_context, perimeter_generation_context *)
{
    assert(user_context != nullptr);
    if (user_context == nullptr)
        return;
    delete static_cast<ModuleState *>(user_context);
}

const perimeter_generation_module_vtable &module_vtable()
{
    static const perimeter_generation_module_vtable vt = {
        &module_start,
        nullptr,
        &module_after,
        &module_end
    };
    return vt;
}

} // namespace

OnlyOnePerimeterOnTop &
OnlyOnePerimeterOnTop::instance(orchestrator_handle *orch)
{
    static OnlyOnePerimeterOnTop s_instance(orch);
    return s_instance;
}

const char *OnlyOnePerimeterOnTop::id_impl() const noexcept
{
    return k_only_one_perimeter_on_top_id;
}

slicing_step_t OnlyOnePerimeterOnTop::step_impl() const noexcept
{
    return PERIMETER_GENERATION_MODULE;
}

const char *const *OnlyOnePerimeterOnTop::dependencies_impl() const noexcept
{
    return k_no_dependencies;
}

int32_t OnlyOnePerimeterOnTop::priority_impl() const noexcept
{
    return 5;
}

int32_t OnlyOnePerimeterOnTop::used_config_keys(raw_used_config_key *keys) const noexcept
{
    if (keys != nullptr)
        for (uint32_t idx = 0; idx < sizeof(k_used_config_keys) / sizeof(k_used_config_keys[0]); ++idx)
            keys[idx] = k_used_config_keys[idx];
    return int32_t(sizeof(k_used_config_keys) / sizeof(k_used_config_keys[0]));
}

const char *OnlyOnePerimeterOnTop::progress_message_format_impl() const noexcept
{
    return "Only one perimeter on top: %u / %u";
}

void OnlyOnePerimeterOnTop::run_impl(const plugin_run_context *run_ctx) const
{
    run_ctx_perimeter_generation_module *ctx = plugin_ctx_as_perimeter_generation_module(run_ctx);
    if (ctx == nullptr)
        return;

    ctx->module.ctx = const_cast<OnlyOnePerimeterOnTop *>(this);
    ctx->module.vt = &module_vtable();
}

void register_only_one_perimeter_on_top_plugin(orchestrator_handle *orch)
{
    orchestrator_register_plugin(orch, OnlyOnePerimeterOnTop::instance(orch).c_instance());
}

}}} // namespace slic3r_api::Perimeter::OnlyOnePerimeterOnTopPlugin

#ifdef ONLY_ONE_PERIMETER_ON_TOP_PLUGIN_DLL
extern "C" void register_plugin(orchestrator_handle *orch)
{
    slic3r_api::Perimeter::OnlyOnePerimeterOnTopPlugin::register_only_one_perimeter_on_top_plugin(orch);
}
#endif // ONLY_ONE_PERIMETER_ON_TOP_PLUGIN_DLL
