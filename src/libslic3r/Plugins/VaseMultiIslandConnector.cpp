///|/ Copyright (c) SuperSlicer 2026 Durand Remi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/

#include "VaseMultiIslandConnector.hpp"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <limits>
#include <map>
#include <sstream>
#include <utility>
#include <vector>

#include "libslic3r/Api/plugin/c/slic3r_orchestrator.h"
#include "libslic3r/Api/plugin/c/steps/slic3r_step_post_slicing.h"
#include "libslic3r/Api/plugin/cpp/ClipperViews.hpp"
#include "libslic3r/Api/plugin/cpp/DataTreeViews.hpp"
#include "libslic3r/Api/plugin/cpp/PrintHelpers.hpp"
#include "libslic3r/Api/plugin/cpp/Views.hpp"

namespace slic3r_api { namespace VaseMultiIslandConnectorPlugin {
namespace {

const char *k_vase_multi_island_connector_id = "vase.multi_island_connector";
const char *k_no_dependencies[] = { nullptr };

const char *k_spiral_vase_key = "spiral_vase";
const char *k_external_perimeter_width_key = "external_perimeter_extrusion_width";
const char *k_perimeter_width_key = "perimeter_extrusion_width";
const char *k_extrusion_width_key = "extrusion_width";
const char *k_first_layer_extrusion_width_key = "first_layer_extrusion_width";
const char *k_nozzle_diameter_key = "nozzle_diameter";

const raw_used_config_key k_used_config_keys[] = {
    { k_spiral_vase_key, RAW_CO_BOOL, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE },
    { k_external_perimeter_width_key, RAW_CO_FLOAT_OR_PERCENT, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE },
    { k_perimeter_width_key, RAW_CO_FLOAT_OR_PERCENT, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE },
    { k_extrusion_width_key, RAW_CO_FLOAT_OR_PERCENT, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE },
    { k_first_layer_extrusion_width_key, RAW_CO_FLOAT_OR_PERCENT, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE },
    { k_nozzle_diameter_key, RAW_CO_VECTOR_FLOAT, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE }
};

struct BoundarySegment;
struct ClosestPoints;
struct IslandInfo;
struct BridgeEdge;
struct UnionFind;
struct ConnectedComponent;

// Query the print-level spiral vase switch. The plugin is deliberately a
// no-op when vase mode is disabled so it can stay active in normal profiles.
bool spiral_vase_enabled(const Print &print);

// Return the largest external perimeter width used by any region on the layer.
// A mixed-region layer needs the widest capsule to keep the go/return vase path
// printable everywhere it crosses the bridge.
coord_t max_external_perimeter_width(const Layer &layer);

// Copy current island slices into storage-owned collections. The layer may be
// rebuilt after the plugin writes LayerRegion slices, so long-lived data must
// not borrow from the original layer.
std::vector<IslandInfo> copy_layer_islands(storage_handle *storage, const Layer &layer);

// Build the list of island pairs close enough to be connected. Each edge stores
// the nearest boundary points, which are also the bridge capsule centerline.
std::vector<BridgeEdge> proximity_edges(const std::vector<IslandInfo> &islands, coord_t threshold);

// Select a non-redundant set of close bridges. Kruskal gives one minimal bridge
// tree per connected component, instead of connecting every close pair.
std::vector<BridgeEdge> minimum_bridge_edges(uint32_t island_count, std::vector<BridgeEdge> edges);

// Offset a two-point centerline into a round-ended bridge capsule. The returned
// collection may contain several polygons after Clipper cleanup, although the
// common case is one ExPolygon.
StoredExPolygonCollection bridge_capsule(storage_handle *storage,
                                         const BridgeEdge &edge,
                                         coord_t bridge_width);

// Merge copied island slices and bridge capsules into component geometries.
std::vector<ConnectedComponent> build_connected_components(storage_handle *storage,
                                                           const std::vector<IslandInfo> &islands,
                                                           std::vector<BridgeEdge> &accepted_edges,
                                                           coord_t bridge_width,
                                                           const plugin_run_context *run_ctx);

// Choose which component survives when vase mode still has more than one
// disconnected group. Continuity with the previous kept layer wins; otherwise
// the largest component is kept.
uint32_t choose_component_to_keep(storage_handle *storage,
                                  const std::vector<ConnectedComponent> &components,
                                  const ExPolygonCollection &previous_kept);

// Build replacement raw LayerRegion slices from the selected component. Region
// ownership remains mostly unchanged, except bridge capsules are added to the
// region that overlaps the capsule endpoints the most.
std::vector<StoredExPolygonCollection> replacement_region_slices(storage_handle *storage,
                                                                 const Layer &layer,
                                                                 const ConnectedComponent &kept_component,
                                                                 const std::vector<BridgeEdge> &all_accepted_edges);

// Write replacement raw region slices to the host layer and ask the host to
// rebuild layer slices and islands from those raw regions.
void publish_layer_regions(const run_ctx_post_slicing &ctx,
                           uint32_t layer_idx,
                           std::vector<StoredExPolygonCollection> &regions);

// Process one multi-island layer. The previous layer geometry is used only for
// disconnected-component selection.
void process_layer(storage_handle *storage,
                   const run_ctx_post_slicing &ctx,
                   const plugin_run_context *run_ctx,
                   const Layer &layer,
                   uint32_t layer_idx,
                   const ExPolygonCollection &previous_kept);

// Geometry helpers used by the graph builder and testable selection logic.
double collection_area(const ExPolygonCollection &areas);
double intersection_area(storage_handle *storage, const ExPolygonCollection &lhs, const ExPolygonCollection &rhs);
c_bounding_box grow_bbox(c_bounding_box bbox, coord_t delta);
bool bboxes_can_be_close(c_bounding_box lhs, c_bounding_box rhs, coord_t threshold);
std::vector<BoundarySegment> boundary_segments(const ExPolygon &area);
ClosestPoints closest_points_between(const ExPolygon &lhs, const ExPolygon &rhs);
c_point closest_point_on_segment(c_point point, c_point a, c_point b);
double squared_distance(c_point lhs, c_point rhs);

struct BoundarySegment
{
    c_point a = {};
    c_point b = {};
};

struct ClosestPoints
{
    c_point lhs = {};
    c_point rhs = {};
    double distance_squared = std::numeric_limits<double>::infinity();
};

struct IslandInfo
{
    IslandInfo(storage_handle *storage, uint32_t island_index, const ExPolygon &slice) :
        index(island_index),
        area(storage, slice),
        bbox(slice.contour().bounding_box()),
        unscaled_area(slice.area())
    {}

    uint32_t index = 0;
    StoredExPolygonCollection area;
    c_bounding_box bbox = {};
    double unscaled_area = 0.;
};

struct BridgeEdge
{
    uint32_t lhs = 0;
    uint32_t rhs = 0;
    c_point lhs_point = {};
    c_point rhs_point = {};
    double distance_squared = 0.;
    int32_t owner_region = -1;
    StoredExPolygonCollection capsule;

    BridgeEdge(storage_handle *storage) : capsule(storage) {}
    BridgeEdge(BridgeEdge &&) noexcept = default;
    BridgeEdge &operator=(BridgeEdge &&) noexcept = default;
};

struct UnionFind
{
    explicit UnionFind(uint32_t count) : parent(count), rank(count, 0)
    {
        for (uint32_t idx = 0; idx < count; ++idx)
            parent[idx] = idx;
    }

    uint32_t find(uint32_t value)
    {
        if (parent[value] != value)
            parent[value] = find(parent[value]);
        return parent[value];
    }

    bool unite(uint32_t lhs, uint32_t rhs)
    {
        uint32_t root_lhs = find(lhs);
        uint32_t root_rhs = find(rhs);
        if (root_lhs == root_rhs)
            return false;
        if (rank[root_lhs] < rank[root_rhs])
            std::swap(root_lhs, root_rhs);
        parent[root_rhs] = root_lhs;
        if (rank[root_lhs] == rank[root_rhs])
            ++rank[root_lhs];
        return true;
    }

    std::vector<uint32_t> parent;
    std::vector<uint32_t> rank;
};

struct ConnectedComponent
{
    explicit ConnectedComponent(storage_handle *storage) : area(storage) {}
    ConnectedComponent(ConnectedComponent &&) noexcept = default;
    ConnectedComponent &operator=(ConnectedComponent &&) noexcept = default;

    std::vector<uint32_t> island_indices;
    std::vector<uint32_t> bridge_indices;
    StoredExPolygonCollection area;
    double unscaled_area = 0.;
};

bool spiral_vase_enabled(const Print &print)
{
    Config config = print.config();
    return config.has(k_spiral_vase_key) && config.get(k_spiral_vase_key).get_bool();
}

coord_t max_external_perimeter_width(const Layer &layer)
{
    coord_t width = 0;
    for (uint32_t region_idx = 0; region_idx < layer.region_count(); ++region_idx) {
        const c_flow flow = layer.region(region_idx).flow(RAW_EXTRUSION_ROLE_EXTERNAL_PERIMETER);
        width = std::max(width, flow.width);
    }
    return width;
}

std::vector<IslandInfo> copy_layer_islands(storage_handle *storage, const Layer &layer)
{
    std::vector<IslandInfo> islands;
    islands.reserve(layer.island_count());
    for (uint32_t island_idx = 0; island_idx < layer.island_count(); ++island_idx)
        islands.emplace_back(storage, island_idx, layer.island(island_idx).slice());
    return islands;
}

std::vector<BridgeEdge> proximity_edges(const std::vector<IslandInfo> &islands, const coord_t threshold)
{
    std::vector<BridgeEdge> edges;
    const double threshold_squared = double(threshold) * double(threshold);
    for (uint32_t lhs_idx = 0; lhs_idx < islands.size(); ++lhs_idx) {
        for (uint32_t rhs_idx = lhs_idx + 1; rhs_idx < islands.size(); ++rhs_idx) {
            if (!bboxes_can_be_close(islands[lhs_idx].bbox, islands[rhs_idx].bbox, threshold))
                continue;

            const ClosestPoints closest = closest_points_between(islands[lhs_idx].area[0],
                                                                 islands[rhs_idx].area[0]);
            if (closest.distance_squared >= threshold_squared)
                continue;

            BridgeEdge edge(islands[lhs_idx].area.storage());
            edge.lhs = lhs_idx;
            edge.rhs = rhs_idx;
            edge.lhs_point = closest.lhs;
            edge.rhs_point = closest.rhs;
            edge.distance_squared = closest.distance_squared;
            edges.push_back(std::move(edge));
        }
    }
    return edges;
}

std::vector<BridgeEdge> minimum_bridge_edges(const uint32_t island_count, std::vector<BridgeEdge> edges)
{
    std::sort(edges.begin(), edges.end(), [](const BridgeEdge &lhs, const BridgeEdge &rhs) {
        return lhs.distance_squared < rhs.distance_squared;
    });

    UnionFind uf(island_count);
    std::vector<BridgeEdge> accepted;
    accepted.reserve(edges.size());
    for (BridgeEdge &edge : edges)
        if (uf.unite(edge.lhs, edge.rhs))
            accepted.push_back(std::move(edge));
    return accepted;
}

StoredExPolygonCollection bridge_capsule(storage_handle *storage,
                                         const BridgeEdge &edge,
                                         const coord_t bridge_width)
{
    StoredPolyline centerline(storage);
    centerline.push_back(edge.lhs_point);
    centerline.push_back(edge.rhs_point);
    const double radius = double(bridge_width) / 2.0;
    ClipperOperand capsule = clipper_offset(ClipperOperand(storage, centerline.readonly()),
                                            radius,
                                            CLIPPER_JOIN_ROUND,
                                            3.0,
                                            CLIPPER_END_OPEN_ROUND);
    return clipper_union(capsule).to_expolygon_collection();
}

std::vector<ConnectedComponent> build_connected_components(storage_handle *storage,
                                                           const std::vector<IslandInfo> &islands,
                                                           std::vector<BridgeEdge> &accepted_edges,
                                                           const coord_t bridge_width,
                                                           const plugin_run_context *run_ctx)
{
    UnionFind uf(uint32_t(islands.size()));
    for (const BridgeEdge &edge : accepted_edges)
        uf.unite(edge.lhs, edge.rhs);

    std::map<uint32_t, uint32_t> root_to_component;
    std::vector<ConnectedComponent> components;
    for (uint32_t island_idx = 0; island_idx < islands.size(); ++island_idx) {
        const uint32_t root = uf.find(island_idx);
        std::map<uint32_t, uint32_t>::iterator it = root_to_component.find(root);
        if (it == root_to_component.end()) {
            const uint32_t component_idx = uint32_t(components.size());
            it = root_to_component.emplace(root, component_idx).first;
            components.emplace_back(storage);
        }
        components[it->second].island_indices.push_back(island_idx);
    }

    for (uint32_t edge_idx = 0; edge_idx < accepted_edges.size(); ++edge_idx) {
        BridgeEdge &edge = accepted_edges[edge_idx];
        edge.capsule = bridge_capsule(storage, edge, bridge_width);
        if (edge.capsule.empty()) {
            report_warning(run_ctx, "Plugin 'vase.multi_island_connector': skipped a degenerate vase bridge.");
            continue;
        }
        const uint32_t root = uf.find(edge.lhs);
        components[root_to_component[root]].bridge_indices.push_back(edge_idx);
    }

    for (ConnectedComponent &component : components) {
        ClipperOperand joined = ClipperOperand::create_empty(storage);
        for (uint32_t island_idx : component.island_indices)
            joined += ClipperOperand(storage, islands[island_idx].area.readonly());
        for (uint32_t edge_idx : component.bridge_indices)
            joined += ClipperOperand(storage, accepted_edges[edge_idx].capsule.readonly());
        component.area = clipper_union(joined).to_expolygon_collection();
        component.unscaled_area = collection_area(component.area.readonly());
    }
    return components;
}

uint32_t choose_component_to_keep(storage_handle *storage,
                                  const std::vector<ConnectedComponent> &components,
                                  const ExPolygonCollection &previous_kept)
{
    assert(!components.empty());

    uint32_t best_idx = 0;
    double best_continuity_area = 0.;
    if (!previous_kept.empty()) {
        for (uint32_t component_idx = 0; component_idx < components.size(); ++component_idx) {
            const double continuity_area = intersection_area(storage,
                                                             components[component_idx].area.readonly(),
                                                             previous_kept);
            if (continuity_area > best_continuity_area) {
                best_continuity_area = continuity_area;
                best_idx = component_idx;
            }
        }
    }

    if (best_continuity_area > 0.)
        return best_idx;

    for (uint32_t component_idx = 1; component_idx < components.size(); ++component_idx)
        if (components[component_idx].unscaled_area > components[best_idx].unscaled_area)
            best_idx = component_idx;
    return best_idx;
}

std::vector<StoredExPolygonCollection> replacement_region_slices(storage_handle *storage,
                                                                 const Layer &layer,
                                                                 const ConnectedComponent &kept_component,
                                                                 const std::vector<BridgeEdge> &all_accepted_edges)
{
    std::vector<StoredExPolygonCollection> original_regions;
    std::vector<StoredExPolygonCollection> new_regions;
    original_regions.reserve(layer.region_count());
    new_regions.reserve(layer.region_count());
    for (uint32_t region_idx = 0; region_idx < layer.region_count(); ++region_idx) {
        original_regions.push_back(layer.region(region_idx).slices().clone(storage));
        ClipperOperand clipped = clipper_intersection(ClipperOperand(storage, original_regions.back().readonly()),
                                                      ClipperOperand(storage, kept_component.area.readonly()));
        new_regions.push_back(clipped.to_expolygon_collection());
    }

    for (uint32_t edge_idx : kept_component.bridge_indices) {
        const BridgeEdge &edge = all_accepted_edges[edge_idx];
        double best_overlap = 0.;
        uint32_t owner_region = 0;
        for (uint32_t region_idx = 0; region_idx < original_regions.size(); ++region_idx) {
            const double overlap = intersection_area(storage,
                                                     original_regions[region_idx].readonly(),
                                                     edge.capsule.readonly());
            if (overlap > best_overlap) {
                best_overlap = overlap;
                owner_region = region_idx;
            }
        }

        // The capsule is real slice material. It is added to exactly one
        // LayerRegion so raw regions stay non-overlapping after reconstruction.
        for (uint32_t region_idx = 0; region_idx < new_regions.size(); ++region_idx) {
            if (region_idx == owner_region)
                continue;
            new_regions[region_idx] =
                clipper_diff(ClipperOperand(storage, new_regions[region_idx].readonly()),
                             ClipperOperand(storage, edge.capsule.readonly())).to_expolygon_collection();
        }
        new_regions[owner_region].append_copy_from(edge.capsule.readonly());
        new_regions[owner_region] =
            clipper_union(ClipperOperand(storage, new_regions[owner_region].readonly())).to_expolygon_collection();
    }

    return new_regions;
}

void publish_layer_regions(const run_ctx_post_slicing &ctx,
                           const uint32_t layer_idx,
                           std::vector<StoredExPolygonCollection> &regions)
{
    layer_handle *mutable_layer = ctx.object_borrow_mutable_layer(ctx.object, layer_idx);
    if (mutable_layer == nullptr)
        return;

    for (uint32_t region_idx = 0; region_idx < regions.size(); ++region_idx) {
        layer_region_handle *mutable_region = layer_get_region_mutable(mutable_layer, region_idx);
        expolygon_collection_handle *raw_slices = ctx.layer_region_borrow_mutable_slices(mutable_region);
        if (raw_slices != nullptr)
            expolygons_move(raw_slices, regions[region_idx].mutable_handle());
    }

    ctx.layer_recompute_slices_and_islands_from_layer_region(mutable_layer);
}

void process_layer(storage_handle *storage,
                   const run_ctx_post_slicing &ctx,
                   const plugin_run_context *run_ctx,
                   const Layer &layer,
                   const uint32_t layer_idx,
                   const ExPolygonCollection &previous_kept)
{
    if (layer.island_count() <= 1)
        return;

    const coord_t external_width = max_external_perimeter_width(layer);
    if (external_width <= 0)
        return;

    const coord_t bridge_width = 2 * external_width;
    const coord_t threshold = 2 * external_width;
    std::vector<IslandInfo> islands = copy_layer_islands(storage, layer);
    std::vector<BridgeEdge> edges = minimum_bridge_edges(uint32_t(islands.size()),
                                                         proximity_edges(islands, threshold));
    std::vector<ConnectedComponent> components = build_connected_components(storage,
                                                                            islands,
                                                                            edges,
                                                                            bridge_width,
                                                                            run_ctx);
    if (components.empty())
        return;

    const uint32_t keep_idx = choose_component_to_keep(storage, components, previous_kept);
    ConnectedComponent &kept_component = components[keep_idx];

    // Even when no bridge was required, disconnected components are still
    // reduced to one kept component so vase mode receives one island per layer.
    std::vector<StoredExPolygonCollection> new_regions =
        replacement_region_slices(storage, layer, kept_component, edges);
    publish_layer_regions(ctx, layer_idx, new_regions);
}

double collection_area(const ExPolygonCollection &areas)
{
    double out = 0.;
    for (const ExPolygon &area : areas)
        out += std::abs(area.area());
    return out;
}

double intersection_area(storage_handle *storage, const ExPolygonCollection &lhs, const ExPolygonCollection &rhs)
{
    if (lhs.empty() || rhs.empty())
        return 0.;
    StoredExPolygonCollection intersection =
        clipper_intersection(ClipperOperand(storage, lhs), ClipperOperand(storage, rhs)).to_expolygon_collection();
    return collection_area(intersection.readonly());
}

c_bounding_box grow_bbox(c_bounding_box bbox, const coord_t delta)
{
    bbox.min.x -= delta;
    bbox.min.y -= delta;
    bbox.max.x += delta;
    bbox.max.y += delta;
    return bbox;
}

bool bboxes_can_be_close(c_bounding_box lhs, c_bounding_box rhs, const coord_t threshold)
{
    lhs = grow_bbox(lhs, threshold);
    return !(lhs.max.x < rhs.min.x || rhs.max.x < lhs.min.x || lhs.max.y < rhs.min.y || rhs.max.y < lhs.min.y);
}

std::vector<BoundarySegment> boundary_segments(const ExPolygon &area)
{
    std::vector<BoundarySegment> segments;
    const auto append_polygon = [&segments](const Polygon &polygon) {
        if (polygon.size() < 2)
            return;
        for (uint32_t idx = 0; idx < polygon.size(); ++idx)
            segments.push_back(BoundarySegment{polygon[idx], polygon[(idx + 1) % polygon.size()]});
    };

    append_polygon(area.contour());
    for (uint32_t hole_idx = 0; hole_idx < area.hole_size(); ++hole_idx)
        append_polygon(area.hole(hole_idx));
    return segments;
}

ClosestPoints closest_points_between(const ExPolygon &lhs, const ExPolygon &rhs)
{
    const std::vector<BoundarySegment> lhs_segments = boundary_segments(lhs);
    const std::vector<BoundarySegment> rhs_segments = boundary_segments(rhs);
    ClosestPoints best;

    for (const BoundarySegment &lhs_segment : lhs_segments) {
        for (const BoundarySegment &rhs_segment : rhs_segments) {
            const c_point rhs_to_lhs_a = closest_point_on_segment(lhs_segment.a, rhs_segment.a, rhs_segment.b);
            double distance_squared = squared_distance(lhs_segment.a, rhs_to_lhs_a);
            if (distance_squared < best.distance_squared)
                best = ClosestPoints{lhs_segment.a, rhs_to_lhs_a, distance_squared};

            const c_point rhs_to_lhs_b = closest_point_on_segment(lhs_segment.b, rhs_segment.a, rhs_segment.b);
            distance_squared = squared_distance(lhs_segment.b, rhs_to_lhs_b);
            if (distance_squared < best.distance_squared)
                best = ClosestPoints{lhs_segment.b, rhs_to_lhs_b, distance_squared};

            const c_point lhs_to_rhs_a = closest_point_on_segment(rhs_segment.a, lhs_segment.a, lhs_segment.b);
            distance_squared = squared_distance(lhs_to_rhs_a, rhs_segment.a);
            if (distance_squared < best.distance_squared)
                best = ClosestPoints{lhs_to_rhs_a, rhs_segment.a, distance_squared};

            const c_point lhs_to_rhs_b = closest_point_on_segment(rhs_segment.b, lhs_segment.a, lhs_segment.b);
            distance_squared = squared_distance(lhs_to_rhs_b, rhs_segment.b);
            if (distance_squared < best.distance_squared)
                best = ClosestPoints{lhs_to_rhs_b, rhs_segment.b, distance_squared};
        }
    }
    return best;
}

c_point closest_point_on_segment(const c_point point, const c_point a, const c_point b)
{
    const double vx = double(b.x - a.x);
    const double vy = double(b.y - a.y);
    const double len_sq = vx * vx + vy * vy;
    if (len_sq <= 0.)
        return a;

    const double t = std::clamp(((double(point.x - a.x) * vx) + (double(point.y - a.y) * vy)) / len_sq,
                                0.0,
                                1.0);
    return c_point{
        coord_t(std::llround(double(a.x) + t * vx)),
        coord_t(std::llround(double(a.y) + t * vy))
    };
}

double squared_distance(const c_point lhs, const c_point rhs)
{
    const double dx = double(lhs.x - rhs.x);
    const double dy = double(lhs.y - rhs.y);
    return dx * dx + dy * dy;
}

} // namespace

VaseMultiIslandConnector &VaseMultiIslandConnector::instance(orchestrator_handle *orch)
{
    static VaseMultiIslandConnector s_instance(orch);
    return s_instance;
}

const char *VaseMultiIslandConnector::id_impl() const noexcept { return k_vase_multi_island_connector_id; }

const char *VaseMultiIslandConnector::name_impl() const noexcept { return "Vase multi-island connector"; }

const char *VaseMultiIslandConnector::description_impl() const noexcept
{
    return "Connects nearby islands before perimeter generation so spiral vase mode can keep one continuous island.";
}

slicing_step_t VaseMultiIslandConnector::step_impl() const noexcept { return STEP_POST_SLICING; }

const char *const *VaseMultiIslandConnector::dependencies_impl() const noexcept { return k_no_dependencies; }

int32_t VaseMultiIslandConnector::priority_impl() const noexcept { return 1000; }

int32_t VaseMultiIslandConnector::used_config_keys(raw_used_config_key *keys) const noexcept
{
    if (keys != nullptr)
        for (uint32_t idx = 0; idx < sizeof(k_used_config_keys) / sizeof(k_used_config_keys[0]); ++idx)
            keys[idx] = k_used_config_keys[idx];
    return int32_t(sizeof(k_used_config_keys) / sizeof(k_used_config_keys[0]));
}

const char *VaseMultiIslandConnector::progress_message_format_impl() const noexcept
{
    return "Connecting vase islands: %u / %u layers";
}

void VaseMultiIslandConnector::setup_run_impl(const plugin_run_context *run_ctx) const
{
    const run_ctx_post_slicing *ctx = plugin_ctx_as_post_slicing(run_ctx);
    if (ctx == nullptr || ctx->object == nullptr)
        return;

    Object object(ctx->object);
    Print print(ctx->print);
    if (spiral_vase_enabled(print))
        progress().add_max(object.layer_count());
}

void VaseMultiIslandConnector::run_impl(const plugin_run_context *run_ctx) const
{
    const run_ctx_post_slicing *ctx = plugin_ctx_as_post_slicing(run_ctx);
    if (ctx == nullptr || ctx->object == nullptr)
        return;

    Object object(ctx->object);
    Print print(ctx->print);
    storage_handle *storage = run_ctx->plugin_storage;
    if (!spiral_vase_enabled(print)) {
        progress().finish_run();
        return;
    }

    StoredExPolygonCollection empty_previous(storage);
    for (uint32_t layer_idx = 0; layer_idx < object.layer_count(); ++layer_idx) {
        throw_if_cancelled(run_ctx);
        Layer layer = object.layer(layer_idx);
        if (layer.island_count() <= 1) {
            progress().increment();
            continue;
        }
        const ExPolygonCollection previous_kept = layer_idx > 0 ?
            object.layer(layer_idx - 1).slices() :
            empty_previous.readonly();
        process_layer(storage, *ctx, run_ctx, layer, layer_idx, previous_kept);
        progress().increment();
    }
    progress().finish_run();
}

void register_vase_multi_island_connector_plugin(orchestrator_handle *orch)
{
    orchestrator_register_plugin(orch, VaseMultiIslandConnector::instance(orch).c_instance());
}

}} // namespace slic3r_api::VaseMultiIslandConnectorPlugin
