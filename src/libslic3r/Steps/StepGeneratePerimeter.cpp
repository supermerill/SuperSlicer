///|/ Copyright (c) SuperSlicer 2026 Durand Rémi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/

#include "StepGeneratePerimeter.hpp"

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <iterator>
#include <memory>
#include <utility>
#include <vector>

#include "libslic3r/Api/host/Orchestrator.hpp"
#include "libslic3r/Api/host/Plugin.hpp"
#include "libslic3r/Api/internal/LayerIslandAccess.hpp"
#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/ExPolygon.hpp"
#include "libslic3r/ExtrusionEntityCollection.hpp"
#include "libslic3r/Layer.hpp"
#include "libslic3r/LayerRegion.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintObject.hpp"
#include "libslic3r/PrintRegion.hpp"
#include "libslic3r/Steps/StepPipeline.hpp"
#include "libslic3r/SurfaceCollection.hpp"

namespace Slic3r::Steps::StepGeneratePerimeter {
namespace {

// STEP_PERIMETER turns sliced layer islands into perimeter extrusion trees and
// island-level fill domains.
//
// The host owns the traversal and the data-tree writes. A selected
// STEP_PERIMETER plugin owns the actual perimeter geometry algorithm. The plugin
// receives a run_ctx_generate_perimeter payload and normally calls
// run_region_group() once for every compatible set of LayerRegion settings it
// wants to process together. Each region group creates or reuses one
// LayerRegionIsland, so later steps can still know which regions and extruder
// produced each set of perimeters.
//
// The generation flow for one region group is:
// 1. Build a PerimeterTree whose root is the island slice, or a plugin-provided
//    root area.
// 2. Ask the selected generator to emit one perimeter depth for a node. The
//    generator writes extrusion into that node and returns the inner areas that
//    become child nodes.
// 3. Run PERIMETER_GENERATION_MODULE plugins around each node generation. These
//    modules implement local policies such as adding extra perimeters, limiting
//    perimeters in some regions, or splitting contour/hole growth.
// 4. Repeat until every node either reached its requested perimeter count or has
//    no printable child area.
// 5. Publish the generated tree into the LayerRegionIsland perimeter bucket and
//    collect the final leaf areas into LayerSliceIsland infill_areas and
//    infill_free_areas.
//
// The step deliberately exposes only narrow callbacks to plugins. Plugins can
// split/rebuild perimeter nodes or publish extrusion through the payload, but
// they do not directly mutate unrelated layer/object state. This keeps the host
// responsible for pointer lifetime, cancellation, tree synchronization, and the
// final data layout consumed by surface generation and infill.
struct PerimeterTreeNode
{
    explicit PerimeterTreeNode(const ExPolygon &area)
        : area(area)
        , infill_areas(area)
        , extrusions(true)
    {}

    // Canonical C ABI node. The C++ wrapper only owns the heavier objects
    // referenced by the handles below; scalar traversal state lives here.
    perimeter_node node = {};

    // Geometry still to process at this tree level. Perimeter generators read
    // this area, emit extrusion for its boundary, then publish smaller child
    // areas through the callback return values.
    ExPolygon area;

    // Fill may be intentionally larger than the strict child area. This keeps
    // the "where infill may anchor into perimeters" domain attached to the
    // node that owns the remaining free area.
    ExPolygon infill_areas;

    // Temporary extrusion output for this node only. The tree is flattened into
    // LayerRegionIsland storage after all perimeter modules have a chance to
    // split or rebuild nodes.
    ExtrusionEntity extrusions;

    // Children are owned as unique_ptr so their addresses stay stable while C
    // ABI callbacks hold raw perimeter_node pointers during one generation run.
    std::vector<std::unique_ptr<PerimeterTreeNode>> children;
    std::vector<perimeter_node *> child_nodes;

    // A generated leaf with no children means the perimeter consumed all of the
    // remaining area. A never-generated leaf means "this area is fill", for
    // example when perimeters=0.
    bool generated_perimeter = false;

    bool needs_more_perimeters() const
    {
        return node.perimeter_needed > 0 && node.perimeter_idx < node.perimeter_needed;
    }

    void sync_c_pointers()
    {
        // The C ABI sees child_nodes.data(), so rebuild this side array every
        // time children are added, removed, or rebuilt. The pointed nodes live
        // in children and keep stable addresses.
        child_nodes.clear();
        child_nodes.reserve(children.size());
        for (std::unique_ptr<PerimeterTreeNode> &child : children) {
            child->node.parent = &node;
            child->sync_c_pointers();
            child_nodes.push_back(&child->node);
        }

        node.area = reinterpret_cast<expolygon_handle *>(&area);
        node.fill_area = reinterpret_cast<expolygon_handle *>(&infill_areas);
        node.extrusions = reinterpret_cast<extrusion_entity_handle *>(&extrusions);
        node.children = child_nodes.empty() ? nullptr : child_nodes.data();
        node.child_count = uint32_t(child_nodes.size());
    }
};

struct PerimeterTree
{
    explicit PerimeterTree(const ExPolygon &root_area, uint32_t perimeter_needed)
        : root(root_area)
    {
        root.node.perimeter_idx = 0;
        root.node.perimeter_needed = perimeter_needed;
        this->sync_c_pointers();
    }

    PerimeterTreeNode root;

    // Scratch span used by split_node_callback(). The callback returns a raw
    // pointer/count pair, so the vector must live after the callback returns.
    std::vector<perimeter_node *> last_span_nodes;

    void sync_c_pointers()
    {
        root.node.parent = nullptr;
        root.sync_c_pointers();
    }
};

struct PerimeterRunContext
{
    // Per-island host context carried through the C callbacks. The perimeter
    // generator plugin receives only the public payload, while callbacks need
    // access to the host tree, selected generator run context, and final output
    // buffers for this island.
    Orchestrator *orchestrator = nullptr;
    Plugin *generator_plugin = nullptr;
    plugin_run_context *generator_run_context = nullptr;
    Print *print = nullptr;
    PrintObject *object = nullptr;
    Layer *layer = nullptr;
    LayerSliceIsland *island = nullptr;
    ExPolygons infill_areas;
    ExPolygons infill_free_areas;
    ExPolygons perimeter_slices;

    // Module host contexts must outlive the module run contexts prepared from
    // them. Store them on the island run context instead of on the stack inside
    // create_perimeter_generation_modules().
    std::vector<plugin_host_context> module_host_contexts;

    // A perimeter generator is expected to call run_region_group(). If it does
    // not, there is nothing safe to publish for this island.
    bool used_region_group = false;
};

struct PerimeterModuleRun
{
    // The module instance is provided by a PERIMETER_GENERATION_MODULE plugin.
    // start() may return a per-run user_context; the same pointer is passed to
    // before/after/end and is owned by the module.
    perimeter_generation_module_instance module = {};
    void *user_context = nullptr;
    bool started = false;
};

LayerRegionIsland *to_region_island(layer_region_island_handle *handle)
{
    return reinterpret_cast<LayerRegionIsland *>(handle);
}

const LayerRegion *to_layer_region(const layer_region_handle *handle)
{
    return reinterpret_cast<const LayerRegion *>(handle);
}

LayerSliceIsland *to_layer_island(layer_island_handle *handle)
{
    return reinterpret_cast<LayerSliceIsland *>(handle);
}

ExtrusionEntity *to_extrusion(extrusion_entity_handle *handle)
{
    return reinterpret_cast<ExtrusionEntity *>(handle);
}

const ExPolygon *to_expolygon(const expolygon_handle *handle)
{
    return reinterpret_cast<const ExPolygon *>(handle);
}

SurfaceCollection *to_surface_collection(surface_collection_handle *handle)
{
    return reinterpret_cast<SurfaceCollection *>(handle);
}

ExtrusionRole bucket_role_from_raw(raw_extrusion_role role)
{
    if ((role & RAW_EXTRUSION_ROLE_THIN) != 0 || role == RAW_EXTRUSION_ROLE_GAP_FILL)
        return LayerRegionIsland::GAP_FILLS;
    if ((role & RAW_EXTRUSION_ROLE_INFILL) != 0)
        return LayerRegionIsland::INFILLS;
    if ((role & RAW_EXTRUSION_ROLE_IRONING) != 0)
        return LayerRegionIsland::IRONINGS;
    if ((role & RAW_EXTRUSION_ROLE_MILL) != 0)
        return LayerRegionIsland::MILLS;
    if ((role & RAW_EXTRUSION_ROLE_SUPPORT) != 0)
        return (role & RAW_EXTRUSION_ROLE_EXTERNAL) != 0 ? LayerRegionIsland::SUPPORT_INTERFACE :
                                                           LayerRegionIsland::SUPPORT;
    return LayerRegionIsland::PERIMETERS;
}

LayerRegionSetCPtrs region_set_from_handles(const layer_region_handle *const *region_handles,
                                             uint32_t region_count,
                                             const LayerSliceIsland &island)
{
    // Plugins may pass an explicit region subset when they want one generated
    // region-island for a compatible group of settings. A null/empty list means
    // "use every region already attached to this layer island".
    LayerRegionSetCPtrs regions;
    for (uint32_t idx = 0; idx < region_count; ++idx) {
        const LayerRegion *region = region_handles == nullptr ? nullptr : to_layer_region(region_handles[idx]);
        if (region != nullptr)
            regions.insert(region);
    }

    if (regions.empty())
        regions = island.regions();
    return regions;
}

uint16_t perimeter_extruder_id(const LayerRegionSetCPtrs &regions)
{
    // LayerRegionIsland is keyed partly by extruder. For now a region group is
    // expected to be compatible enough that the first region defines the
    // perimeter extruder for the whole generated island.
    if (regions.empty())
        return uint16_t(-1);

    const int16_t extruder_id = int16_t((*regions.begin())->region().config().perimeter_extruder) - 1;
    return extruder_id < 0 ? uint16_t(-1) : uint16_t(extruder_id);
}

uint32_t requested_perimeter_count(const LayerRegionSetCPtrs &regions)
{
    // The host initializes the root node with the base perimeter count. Modules
    // may later raise or lower child node counts, but the root starts from the
    // selected region group's normal print setting.
    if (regions.empty())
        return 0;

    const int count = (*regions.begin())->region().config().perimeters.value;
    return count <= 0 ? 0 : uint32_t(count);
}

void append_extrusion_children(ExtrusionEntityCollection &dst, ExtrusionEntity &src)
{
    // Move a plugin-produced subtree into a legacy collection bucket. Nop
    // entities are ignored, collections donate their children, and leaves are
    // appended as single printable entities.
    if (src.is_nop())
        return;

    if (ExtrusionEntityCollection *collection = dynamic_cast<ExtrusionEntityCollection *>(&src)) {
        dst.append_move_from(*collection);
        return;
    }

    if (src.is_leaf()) {
        dst.append(std::move(src));
        return;
    }

    ExtrusionEntity::Children &children = src.children();
    while (!children.empty()) {
        dst.append(std::move(children.front()));
        children.erase(children.begin());
    }
}

void append_extrusion_children(ExtrusionEntity &dst, ExtrusionEntity &src)
{
    // Same transfer as above, but for a generic ExtrusionEntity parent. A leaf
    // cannot donate children, so clone_move() transfers its payload into a new
    // owned child and then clears the source shell.
    if (src.is_nop())
        return;

    if (src.is_leaf()) {
        dst.append_child(ExtrusionEntityUPtr(src.clone_move()));
        src.clear_content();
        src.clear_properties();
        return;
    }

    ExtrusionEntity::Children &children = src.children();
    while (!children.empty()) {
        dst.append_child(std::move(children.front()));
        children.erase(children.begin());
    }
}

void collect_extrusions(PerimeterTreeNode &node, ExtrusionEntityCollection &out)
{
    // Keep the extrusion publication shaped like the generation tree. Each
    // PerimeterTreeNode becomes one collection node containing the loops/gap
    // fill generated for this level, followed by one child collection per
    // inner area. Later steps can then recover the perimeter depth hierarchy
    // instead of receiving a flat list of unrelated loops.
    std::unique_ptr<ExtrusionEntity> group =
        std::make_unique<ExtrusionEntity>(ExtrusionEntity::Children(), false, true, false);
    append_extrusion_children(*group, node.extrusions);
    for (std::unique_ptr<PerimeterTreeNode> &child : node.children) {
        ExtrusionEntityCollection child_group;
        collect_extrusions(*child, child_group);
        append_extrusion_children(*group, child_group);
    }

    if (!group->is_leaf() && group->child_count() > 0)
        out.append(std::move(group));
}

void collect_leaf_areas(const PerimeterTreeNode &node,
                        ExPolygons &infill_areas,
                        ExPolygons &infill_free_areas)
{
    if (!node.children.empty()) {
        for (const std::unique_ptr<PerimeterTreeNode> &child : node.children)
            collect_leaf_areas(*child, infill_areas, infill_free_areas);
        return;
    }

    if (node.area.empty())
        return;

    // A leaf that already went through the perimeter generator has no inner
    // child. This means the generated perimeter consumed the whole remaining
    // area, so it must not be republished as fill. A leaf that was never
    // generated still represents original fill, for example when perimeters=0.
    if (node.generated_perimeter)
        return;

    infill_free_areas.push_back(node.area);
    infill_areas.push_back(node.infill_areas.empty() ? node.area : node.infill_areas);
}

PerimeterTreeNode *find_node(PerimeterTreeNode &node, perimeter_node *c_node)
{
    // C callbacks receive perimeter_node pointers. Walk the owning C++ tree to
    // recover the wrapper node that owns geometry and extrusions.
    if (&node.node == c_node)
        return &node;

    for (std::unique_ptr<PerimeterTreeNode> &child : node.children) {
        PerimeterTreeNode *found = find_node(*child, c_node);
        if (found != nullptr)
            return found;
    }
    return nullptr;
}

PerimeterTreeNode *find_node(PerimeterTree &tree, perimeter_node *c_node)
{
    return c_node == nullptr ? nullptr : find_node(tree.root, c_node);
}

ExPolygon pick_infill_areas_for_child(const ExPolygon &area, const ExPolygons &infill_areas)
{
    // The generator may rebuild children with geometry but without a matching
    // one-to-one fill area list. Use a cheap containment test first; if several
    // fill areas contain the sample point, pick the one with the largest
    // geometric intersection with the child.
    if (area.empty() || infill_areas.empty())
        return area;

    const Point sample = area.contour.front();
    std::vector<size_t> candidate_idxs;
    for (size_t idx = 0; idx < infill_areas.size(); ++idx)
        if (infill_areas[idx].contains(sample))
            candidate_idxs.push_back(idx);

    if (candidate_idxs.size() == 1)
        return infill_areas[candidate_idxs.front()];

    if (candidate_idxs.size() > 1) {
        ExPolygons best_intersection;
        double best_area = 0.;
        for (size_t idx : candidate_idxs) {
            ExPolygons intersection = intersection_ex(ExPolygons{area}, ExPolygons{infill_areas[idx]});
            double intersection_area = 0.;
            for (const ExPolygon &expoly : intersection)
                intersection_area += expoly.area();

            if (intersection_area > best_area) {
                best_area = intersection_area;
                best_intersection = std::move(intersection);
            }
        }

        ExPolygons merged = union_ex(best_intersection);
        if (!merged.empty())
            return merged.front();
    }

    return area;
}

void set_split_node_area(PerimeterTreeNode &node, ExPolygon &&area, const ExPolygons &fill_clip)
{
    // A split node keeps its own free area. Its fill/anchor domain follows the
    // same clip when available, otherwise it falls back to the free area so the
    // downstream fill generation still has a valid domain.
    node.area = std::move(area);
    node.infill_areas = node.area;

    if (fill_clip.empty())
        return;

    ExPolygons fill_intersection = intersection_ex(ExPolygons{node.area}, fill_clip);
    ExPolygons merged = union_ex(fill_intersection);
    if (!merged.empty())
        node.infill_areas = merged.front();
}

std::unique_ptr<PerimeterTreeNode> make_child_node(PerimeterTreeNode &parent,
                                                   const ExPolygon &area,
                                                   const ExPolygon *infill_areas)
{
    // Child nodes represent the next onion shell. The perimeter index increases
    // by one, while the requested count is inherited unless a module changes it
    // later through the public node handle.
    std::unique_ptr<PerimeterTreeNode> child = std::make_unique<PerimeterTreeNode>(area);
    child->node.parent = &parent.node;
    if (infill_areas != nullptr)
        child->infill_areas = *infill_areas;
    child->node.perimeter_idx = parent.node.perimeter_idx + 1;
    child->node.perimeter_needed = parent.node.perimeter_needed;
    child->sync_c_pointers();
    return child;
}

std::unique_ptr<PerimeterTreeNode> make_split_sibling(const PerimeterTreeNode &source)
{
    // Splitting a leaf creates siblings at the same perimeter depth. The new
    // node copies traversal state but receives its own geometry before being
    // inserted next to the source node.
    std::unique_ptr<PerimeterTreeNode> node = std::make_unique<PerimeterTreeNode>(source.area);
    node->node.parent = source.node.parent;
    node->node.perimeter_idx = source.node.perimeter_idx;
    node->node.perimeter_needed = source.node.perimeter_needed;
    node->generated_perimeter = source.generated_perimeter;
    node->sync_c_pointers();
    return node;
}

void create_children(PerimeterTreeNode &parent, const ExPolygons &inner_areas, const ExPolygons &inner_infill_areas)
{
    // Generator callbacks return the free areas for the next perimeter depth.
    // When a matching infill list is supplied, keep those fill domains paired by
    // index; otherwise each child falls back to its own free area.
    parent.children.clear();
    parent.children.reserve(inner_areas.size());
    const bool has_matching_infill_areas = inner_infill_areas.size() == inner_areas.size();
    for (size_t idx = 0; idx < inner_areas.size(); ++idx) {
        if (inner_areas[idx].empty())
            continue;
        const ExPolygon *infill_areas = has_matching_infill_areas ? &inner_infill_areas[idx] : nullptr;
        parent.children.push_back(make_child_node(parent, inner_areas[idx], infill_areas));
    }
    parent.sync_c_pointers();
}

void append_sibling(PerimeterTree &tree, PerimeterTreeNode &node, std::unique_ptr<PerimeterTreeNode> &&sibling)
{
    // The split callback mutates a node already stored in its parent. Insert any
    // extra pieces next to that node, then refresh raw child pointers exposed to
    // C modules.
    PerimeterTreeNode *parent = find_node(tree, node.node.parent);
    assert(parent != nullptr);
    if (parent == nullptr)
        return;
    sibling->node.parent = &parent->node;
    parent->children.push_back(std::move(sibling));
    parent->sync_c_pointers();
}

void split_node_callback(perimeter_generation_context *context,
                         perimeter_node *node,
                         const expolygon_collection_handle *clip,
                         perimeter_node_span *inside_nodes_out)
{
    // Perimeter modules use this callback when a setting applies only to part
    // of a leaf. The callback keeps the inside pieces as the returned span and
    // leaves outside pieces as siblings so the main generation loop will still
    // process every part of the island.
    if (inside_nodes_out != nullptr)
        *inside_nodes_out = {};
    if (context == nullptr || node == nullptr || clip == nullptr || inside_nodes_out == nullptr)
        return;

    PerimeterTree *tree = reinterpret_cast<PerimeterTree *>(context->generator_context);
    PerimeterTreeNode *to_split = tree == nullptr ? nullptr : find_node(*tree, node);
    const ExPolygons *clip_expolygons = reinterpret_cast<const ExPolygons *>(clip);
    if (to_split == nullptr || clip_expolygons == nullptr || clip_expolygons->empty())
        return;

    // Split only leaf nodes. Splitting a node that already has children would
    // need to repartition all generated descendants, which is a different
    // operation from this compact module callback.
    if (!to_split->children.empty() || to_split->node.parent == nullptr)
        return;

    ExPolygons area_yes = intersection_ex(ExPolygons{to_split->area}, *clip_expolygons);
    if (area_yes.empty())
        return;

    ExPolygons area_no = diff_ex(ExPolygons{to_split->area}, area_yes);
    if (area_no.empty()) {
        tree->last_span_nodes = {&to_split->node};
        inside_nodes_out->items = tree->last_span_nodes.data();
        inside_nodes_out->count = uint32_t(tree->last_span_nodes.size());
        return;
    }

    ExPolygons fill_yes = intersection_ex(ExPolygons{to_split->infill_areas}, *clip_expolygons);
    ExPolygons fill_no = diff_ex(ExPolygons{to_split->infill_areas}, fill_yes);

    tree->last_span_nodes.clear();
    ExPolygon first_inside = std::move(area_yes.front());
    area_yes.erase(area_yes.begin());
    set_split_node_area(*to_split, std::move(first_inside), fill_yes);
    tree->last_span_nodes.push_back(&to_split->node);

    while (!area_yes.empty()) {
        std::unique_ptr<PerimeterTreeNode> sibling = make_split_sibling(*to_split);
        ExPolygon inside_area = std::move(area_yes.front());
        area_yes.erase(area_yes.begin());
        set_split_node_area(*sibling, std::move(inside_area), fill_yes);
        tree->last_span_nodes.push_back(&sibling->node);
        append_sibling(*tree, *to_split, std::move(sibling));
    }

    while (!area_no.empty()) {
        std::unique_ptr<PerimeterTreeNode> sibling = make_split_sibling(*to_split);
        ExPolygon outside_area = std::move(area_no.front());
        area_no.erase(area_no.begin());
        set_split_node_area(*sibling, std::move(outside_area), fill_no);
        append_sibling(*tree, *to_split, std::move(sibling));
    }

    tree->sync_c_pointers();
    inside_nodes_out->items = tree->last_span_nodes.data();
    inside_nodes_out->count = uint32_t(tree->last_span_nodes.size());
}

void rebuild_children_callback(perimeter_generation_context *context,
                               perimeter_node *node,
                               const expolygon_collection_handle *areas,
                               const expolygon_collection_handle *infill_areas)
{
    // Modules that know a better child topology may replace all children of a
    // node at once. This is stronger than split_node(): existing child nodes are
    // discarded and the next generation pass follows the provided areas.
    if (context == nullptr || node == nullptr || areas == nullptr)
        return;

    PerimeterTree *tree = reinterpret_cast<PerimeterTree *>(context->generator_context);
    PerimeterTreeNode *parent = tree == nullptr ? nullptr : find_node(*tree, node);
    const ExPolygons *child_areas = reinterpret_cast<const ExPolygons *>(areas);
    const ExPolygons *child_infill_areas = reinterpret_cast<const ExPolygons *>(infill_areas);
    if (parent == nullptr || child_areas == nullptr)
        return;

    parent->children.clear();
    parent->children.reserve(child_areas->size());
    for (const ExPolygon &area : *child_areas) {
        if (area.empty())
            continue;
        ExPolygon infill_areas = child_infill_areas == nullptr || child_infill_areas->empty() ?
                                  area :
                                  pick_infill_areas_for_child(area, *child_infill_areas);
        parent->children.push_back(make_child_node(*parent, area, &infill_areas));
    }
    tree->sync_c_pointers();
}

perimeter_generation_context make_generation_context(plugin_run_context *run_context,
                                                     const run_ctx_generate_perimeter &ctx,
                                                     layer_region_island_handle *region_island,
                                                     PerimeterTree &tree)
{
    // This context is shared by the selected generator and all perimeter
    // modules for one region group. The host owns the tree; plugins only see
    // handles and callbacks that mutate it in controlled ways.
    perimeter_generation_context context = {};
    context.run_ctx = run_context;
    context.print = ctx.print;
    context.object = ctx.object;
    context.layer = ctx.layer;
    context.island = ctx.island;
    context.region_island = region_island;
    context.root = &tree.root.node;
    context.generator_context = &tree;
    context.split_node = &split_node_callback;
    context.rebuild_children = &rebuild_children_callback;
    return context;
}

void call_module_start(std::vector<PerimeterModuleRun> &modules,
                       perimeter_generation_context &context)
{
    // start() is called once per region group. Modules use it to build caches
    // derived from the island, regions, or settings, and end() receives the
    // returned pointer for cleanup.
    for (PerimeterModuleRun &module_run : modules) {
        module_run.started = true;
        if (module_run.module.vt != nullptr && module_run.module.vt->start != nullptr)
            module_run.user_context = module_run.module.vt->start(module_run.module.ctx, &context);
    }
}

void call_module_before(const std::vector<PerimeterModuleRun> &modules,
                        perimeter_generation_context &context,
                        perimeter_node &node)
{
    // before() runs immediately before the generator emits one perimeter for a
    // node. It may split the node or adjust node metadata such as the requested
    // perimeter count.
    for (const PerimeterModuleRun &module_run : modules)
        if (module_run.started && module_run.module.vt != nullptr && module_run.module.vt->before != nullptr)
            module_run.module.vt->before(module_run.module.ctx, module_run.user_context, &context, &node);
}

void call_module_after(const std::vector<PerimeterModuleRun> &modules,
                       perimeter_generation_context &context,
                       perimeter_node &node)
{
    // after() runs after the generator emitted extrusion and child areas for
    // this node. Modules commonly refine the newly-created children or move
    // generated material between perimeter and fill domains.
    for (const PerimeterModuleRun &module_run : modules)
        if (module_run.started && module_run.module.vt != nullptr && module_run.module.vt->after != nullptr)
            module_run.module.vt->after(module_run.module.ctx, module_run.user_context, &context, &node);
}

void call_module_end(std::vector<PerimeterModuleRun> &modules,
                     perimeter_generation_context &context)
{
    // Always pair a successful start() with end(). The module owns any
    // user_context allocation and must release it here before the region group
    // context goes out of scope.
    for (PerimeterModuleRun &module_run : modules) {
        if (module_run.started && module_run.module.vt != nullptr && module_run.module.vt->end != nullptr)
            module_run.module.vt->end(module_run.module.ctx, module_run.user_context, &context);
        module_run.user_context = nullptr;
        module_run.started = false;
    }
}

class PerimeterModuleEndGuard
{
public:
    PerimeterModuleEndGuard(std::vector<PerimeterModuleRun> &modules,
                            perimeter_generation_context &context)
        : m_modules(&modules)
        , m_context(&context)
    {}

    ~PerimeterModuleEndGuard()
    {
        // The generation callback can return early on cancellation or plugin
        // failure. RAII keeps module cleanup paired with start() in all exits.
        this->finish();
    }

    void finish()
    {
        if (m_modules != nullptr && m_context != nullptr)
            call_module_end(*m_modules, *m_context);
        m_modules = nullptr;
        m_context = nullptr;
    }

private:
    std::vector<PerimeterModuleRun> *m_modules = nullptr;
    perimeter_generation_context *m_context = nullptr;
};

std::vector<PerimeterModuleRun>
create_perimeter_generation_modules(PerimeterRunContext &run)
{
    // Perimeter generation modules are normal plugins that expose a secondary
    // vtable instead of doing work directly in their run() method. This setup
    // phase lets each plugin publish that vtable for the current print.
    std::vector<PerimeterModuleRun> modules;
    if (run.orchestrator == nullptr)
        return modules;

    std::vector<Plugin *> plugins = selected_or_active_plugins_for_step(*run.orchestrator,
                                                                        PERIMETER_GENERATION_MODULE,
                                                                        &run.print->full_print_config());
    modules.reserve(plugins.size());
    run.module_host_contexts.clear();
    run.module_host_contexts.reserve(plugins.size());

    for (Plugin *plugin : plugins) {
        run.module_host_contexts.push_back(
            run.orchestrator->prepare_plugin_host_context(PERIMETER_GENERATION_MODULE, plugin, run.print));
        plugin_host_context &host_context = run.module_host_contexts.back();
        plugin_run_context run_context =
            run.orchestrator->prepare_plugin_run_context(PERIMETER_GENERATION_MODULE, plugin, &host_context);
        run_ctx_perimeter_generation_module module_context = {};
        run_context.data = &module_context;

        plugin->setup(run_context, 1);
        plugin->setup_run(run_context);
        plugin->run(run_context);

        if (module_context.module.vt != nullptr) {
            PerimeterModuleRun module_run;
            module_run.module = module_context.module;
            modules.push_back(module_run);
        }
    }

    return modules;
}

void publish_region_group(PerimeterRunContext &run,
                          LayerRegionIsland &region_island,
                          PerimeterTree &tree)
{
    // Once the generator tree is complete, publish it into the real data tree.
    // Perimeter extrusion goes to the region-island bucket; leaf areas are
    // accumulated on the layer island because later surface generation works
    // from island-level fill domains.
    ExtrusionEntityCollection perimeters;
    perimeters.set_can_sort_reverse(false, false);
    collect_extrusions(tree.root, perimeters);

    ExtrusionEntityCollection &dst = region_island.mutable_extrusion(LayerRegionIsland::PERIMETERS);
    dst.clear();
    dst.append_move_from(perimeters);
    region_island.remove_empty_extrusions();

    collect_leaf_areas(tree.root, run.infill_areas, run.infill_free_areas);
}

int32_t run_region_group_callback(const run_ctx_generate_perimeter *ctx,
                                  const layer_region_handle *const *region_handles,
                                  uint32_t region_count,
                                  const expolygon_handle *root_area,
                                  void *generator_context,
                                  perimeter_generate_node_fn generate_node)
{
    // This is the main host-side execution loop for a generator-selected region
    // group. The plugin supplies generate_node(), while the host owns traversal,
    // module calls, cancellation checks, and final publication.
    if (ctx == nullptr || ctx->host_context == nullptr || generate_node == nullptr)
        return 0;

    PerimeterRunContext &run = *reinterpret_cast<PerimeterRunContext *>(ctx->host_context);
    if (run.island == nullptr || run.generator_run_context == nullptr)
        return 0;
    run.used_region_group = true;

    const ExPolygon *root_expolygon = root_area == nullptr ? &run.island->get_slice() : to_expolygon(root_area);
    if (root_expolygon == nullptr || root_expolygon->empty())
        return 1;

    LayerRegionSetCPtrs regions = region_set_from_handles(region_handles, region_count, *run.island);
    if (regions.empty())
        return 0;

    // One region group maps to one LayerRegionIsland. Generators can call this
    // callback multiple times with different compatible region sets; each group
    // then owns independent extrusion and fill surfaces.
    LayerRegionIsland &region_island =
        run.island->get_or_add_region_island(regions, perimeter_extruder_id(regions));
    PerimeterTree tree(*root_expolygon, requested_perimeter_count(regions));
    std::vector<PerimeterModuleRun> modules = create_perimeter_generation_modules(run);
    perimeter_generation_context generation_context =
        make_generation_context(run.generator_run_context,
                                *ctx,
                                reinterpret_cast<layer_region_island_handle *>(&region_island),
                                tree);

    PerimeterModuleEndGuard module_end_guard(modules, generation_context);
    call_module_start(modules, generation_context);

    int32_t result = 1;
    std::vector<PerimeterTreeNode *> pending_nodes;
    pending_nodes.push_back(&tree.root);
    while (!pending_nodes.empty()) {
        // Depth-first traversal keeps memory small and naturally follows the
        // onion-shell tree. The order of publication is reconstructed later by
        // collect_extrusions(), so the pending stack only controls generation.
        PerimeterTreeNode *node = pending_nodes.back();
        pending_nodes.pop_back();
        if (node == nullptr || !node->needs_more_perimeters())
            continue;

        if (run.print != nullptr)
            run.print->throw_if_canceled();

        tree.sync_c_pointers();
        call_module_before(modules, generation_context, node->node);

        node->extrusions.clear_content();
        node->extrusions.clear_properties();
        ExPolygons inner_areas;
        ExPolygons inner_infill_areas;
        // The generator emits exactly one perimeter depth for this node. It
        // returns child areas for the next depth instead of mutating host
        // containers directly.
        const int32_t ok = generate_node(generator_context,
                                         &generation_context,
                                         &node->node,
                                         reinterpret_cast<expolygon_collection_handle *>(&inner_areas),
                                         reinterpret_cast<expolygon_collection_handle *>(&inner_infill_areas));
        if (!ok) {
            result = 0;
            break;
        }
        node->generated_perimeter = true;

        create_children(*node, inner_areas, inner_infill_areas);

        tree.sync_c_pointers();
        call_module_after(modules, generation_context, node->node);

        // Modules may have rebuilt or split children during after(), so iterate
        // over the final child list that exists after all module callbacks.
        for (std::unique_ptr<PerimeterTreeNode> &child : node->children)
            pending_nodes.push_back(child.get());
    }

    module_end_guard.finish();
    if (!result)
        return result;

    publish_region_group(run, region_island, tree);
    return result;
}

layer_region_island_handle *get_or_create_region_island_callback(const layer_island_handle *island_handle,
                                                                 const layer_region_handle *const *region_handles,
                                                                 uint32_t region_count)
{
    // Generators may need to create a region-island without running a full
    // region group, for example to publish auxiliary extrusion. The callback
    // keeps the same region/extruder grouping rule as run_region_group().
    LayerSliceIsland *island = to_layer_island(const_cast<layer_island_handle *>(island_handle));
    if (island == nullptr)
        return nullptr;

    LayerRegionSetCPtrs regions = region_set_from_handles(region_handles, region_count, *island);
    if (regions.empty())
        return nullptr;
    LayerRegionIsland &region_island = island->get_or_add_region_island(regions, perimeter_extruder_id(regions));
    return reinterpret_cast<layer_region_island_handle *>(&region_island);
}

int32_t set_region_island_extrusion_callback(layer_region_island_handle *region_island_handle,
                                             raw_extrusion_role role,
                                             extrusion_entity_handle *extrusion_handle)
{
    // Direct publication is reserved for generator-owned buckets. The raw role
    // is mapped to the LayerRegionIsland bucket so plugins can use public role
    // flags without knowing the host storage enum.
    LayerRegionIsland *region_island = to_region_island(region_island_handle);
    if (region_island == nullptr)
        return 0;

    ExtrusionEntityCollection &dst = region_island->mutable_extrusion(bucket_role_from_raw(role));
    dst.clear();
    if (extrusion_handle != nullptr)
        append_extrusion_children(dst, *to_extrusion(extrusion_handle));
    region_island->remove_empty_extrusions();
    return 1;
}

void clear_island_outputs(LayerSliceIsland &island)
{
    // Perimeter generation owns these island outputs. Cleaning them before a
    // rerun prevents stale region-islands, fill areas, or perimeter slices from
    // surviving when a plugin produces less geometry than the previous run.
    island.mutable_regions_islands().clear();
    ApiInternal::LayerIslandAccess::set_infill_areas(island, ExPolygons{});
    ApiInternal::LayerIslandAccess::infill_free_areas_mutable(island).clear();
    ApiInternal::LayerIslandAccess::perimeter_slices_mutable(island).clear();
}

void clear_layer_outputs(Layer &layer)
{
    for (LayerSliceIsland &island : layer.islands())
        clear_island_outputs(island);
}

void assign_island_outputs(LayerSliceIsland &island, PerimeterRunContext &run)
{
    // The step stores two related fill domains:
    // - infill_free_areas: strict remaining free space after perimeters;
    // - infill_areas: print domain that may overlap perimeters for anchoring.
    // Both are validated here because plugins may emit many small child pieces.
    run.infill_areas = ensure_valid(std::move(run.infill_areas));
    run.infill_free_areas = ensure_valid(std::move(run.infill_free_areas));

    ApiInternal::LayerIslandAccess::set_infill_areas(island, std::move(run.infill_areas));
    ApiInternal::LayerIslandAccess::infill_free_areas_mutable(island) =
        std::move(run.infill_free_areas);
    ApiInternal::LayerIslandAccess::perimeter_slices_mutable(island) =
        union_ex(ExPolygons{island.get_slice()});
}

size_t count_layer_islands(const Print &print)
{
    // setup() receives the total number of islands so plugins can initialize
    // progress bars once before run_step() iterates object/layer/island.
    size_t count = 0;
    for (const PrintObject &object : print.objects())
        for (const Layer &layer : object.layers())
            count += layer.islands().size();
    return count;
}

bool run_generator_for_island(Orchestrator &orchestrator,
                              Plugin &plugin,
                              Print &print,
                              PrintObject &object,
                              Layer &layer,
                              LayerSliceIsland &island,
                              plugin_host_context &host_context)
{
    // Build the STEP_PERIMETER payload for one island. The selected generator
    // should call run_region_group(); if it does not, used_region_group stays
    // false and the caller deliberately leaves this island without new output.
    plugin_run_context run_context =
        orchestrator.prepare_plugin_run_context(STEP_PERIMETER, &plugin, &host_context);

    PerimeterRunContext perimeter_context;
    perimeter_context.orchestrator = &orchestrator;
    perimeter_context.generator_plugin = &plugin;
    perimeter_context.generator_run_context = &run_context;
    perimeter_context.print = &print;
    perimeter_context.object = &object;
    perimeter_context.layer = &layer;
    perimeter_context.island = &island;

    run_ctx_generate_perimeter payload = {};
    payload.print = reinterpret_cast<const print_handle *>(&print);
    payload.object = reinterpret_cast<const object_handle *>(&object);
    payload.layer = reinterpret_cast<const layer_handle *>(&layer);
    payload.island = reinterpret_cast<const layer_island_handle *>(&island);
    payload.host_context = &perimeter_context;
    payload.run_region_group = &run_region_group_callback;
    payload.get_or_create_region_island = &get_or_create_region_island_callback;
    payload.set_region_island_extrusion = &set_region_island_extrusion_callback;
    run_context.data = &payload;

    plugin.setup_run(run_context);
    plugin.run(run_context);

    if (perimeter_context.used_region_group)
        assign_island_outputs(island, perimeter_context);
    return perimeter_context.used_region_group;
}

} // namespace

void clean_and_prepare(Print &print)
{
    // The new pipeline currently treats perimeter generation as owning all
    // perimeter-era island outputs, so invalidation is intentionally coarse.
    // Finer invalidation can keep unaffected islands later.
    for (PrintObject &object : print.objects())
        for (Layer &layer : object.layers())
            clear_layer_outputs(layer);
}

bool validate_pre(const Print &, std::string *)
{
    // The step can start from plain sliced islands. Detailed geometry checks
    // live in later post-condition validators where generated data exists.
    return true;
}

bool validate_post(const Print &, std::string *)
{
    // Perimeter output validation is still exercised by focused plugin tests.
    // Keep the production validator cheap until the new pipeline contracts are
    // stable enough to enforce globally.
    return true;
}

void run_step(Orchestrator &orchestrator, Print &print)
{
    // STEP_PERIMETER is an exclusive step: many perimeter generator plugins may
    // be active, but exactly one owns the generation for this print. The
    // selected_or_active_plugin_for_step() helper reads the generated
    // step_perimeter_plugin config option when it exists, falling back to the
    // first active generator only when there is no selector to read.
    Plugin *plugin = selected_or_active_plugin_for_step(orchestrator, STEP_PERIMETER, &print.full_print_config());
    if (plugin == nullptr)
        return;

    const size_t run_count = count_layer_islands(print);
    plugin_host_context host_context =
        orchestrator.prepare_plugin_host_context(STEP_PERIMETER, plugin, &print);
    host_context.object_count = run_count;
    plugin_run_context setup_context =
        orchestrator.prepare_plugin_run_context(STEP_PERIMETER, plugin, &host_context);
    plugin->setup(setup_context, uint32_t(run_count));

    for (size_t object_idx = 0; object_idx < print.objects().size(); ++object_idx) {
        host_context.object_idx = object_idx;
        PrintObject &object = print.object(object_idx);
        for (Layer &layer : object.layers()) {
            for (LayerSliceIsland &island : layer.islands()) {
                if (setup_context.is_cancelled != nullptr && setup_context.is_cancelled(setup_context.host_context))
                    return;
                run_generator_for_island(orchestrator, *plugin, print, object, layer, island, host_context);
            }
        }
    }

}

} // namespace Slic3r::Steps::StepGeneratePerimeter
