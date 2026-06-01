#include "perimeter_test_helpers.hpp"

#include <catch2/catch.hpp>

#include "plugin_test_helpers.hpp"
#include "test_data.hpp"

#include "libslic3r/Api/host/Orchestrator.hpp"
#include "libslic3r/Api/host/Plugin.hpp"
#include "libslic3r/Api/internal/LayerAccess.hpp"
#include "libslic3r/Api/internal/LayerRegionAccess.hpp"
#include "libslic3r/Api/plugin/c/steps/slic3r_step_perimeter.h"
#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/Layer.hpp"
#include "libslic3r/LayerRegion.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/PrintObject.hpp"
#include "libslic3r/Steps/StepGeneratePerimeter.hpp"
#include "libslic3r/Steps/StepLayerHeightGeneration.hpp"
#include "libslic3r/Steps/StepPostPerimeterGeneration.hpp"
#include "libslic3r/Steps/StepPostSlicing.hpp"
#include "libslic3r/Steps/StepSlicing.hpp"
#include "libslic3r/TriangleMesh.hpp"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <memory>
#include <utility>

namespace Slic3r::Test::PerimeterPluginTests {
namespace {

struct Box
{
    double min_x = 0.;
    double min_y = 0.;
    double min_z = 0.;
    double max_x = 0.;
    double max_y = 0.;
    double max_z = 0.;
};

class ScopedActivePlugins
{
public:
    explicit ScopedActivePlugins(std::initializer_list<const char *> plugin_ids) :
        m_orchestrator(Orchestrator::instance())
    {
        m_previous_active_plugins.reserve(m_orchestrator.active_plugins().size());
        for (Plugin *plugin : m_orchestrator.active_plugins())
            m_previous_active_plugins.push_back(plugin);

        m_orchestrator.clear_active_plugins();
        for (const char *plugin_id : plugin_ids) {
            INFO("Activating test plugin " << plugin_id);
            REQUIRE(m_orchestrator.set_plugin_active(plugin_id, true));
        }
    }

    ~ScopedActivePlugins()
    {
        m_orchestrator.clear_active_plugins();
        for (Plugin *plugin : m_previous_active_plugins)
            m_orchestrator.set_plugin_active(plugin, true);
    }

private:
    Orchestrator &m_orchestrator;
    std::vector<Plugin *> m_previous_active_plugins;
};

TriangleMesh make_box(const Box &box)
{
    std::vector<Vec3f> vertices = {
        {float(box.min_x), float(box.min_y), float(box.min_z)},
        {float(box.max_x), float(box.min_y), float(box.min_z)},
        {float(box.max_x), float(box.max_y), float(box.min_z)},
        {float(box.min_x), float(box.max_y), float(box.min_z)},
        {float(box.min_x), float(box.min_y), float(box.max_z)},
        {float(box.max_x), float(box.min_y), float(box.max_z)},
        {float(box.max_x), float(box.max_y), float(box.max_z)},
        {float(box.min_x), float(box.max_y), float(box.max_z)}
    };
    std::vector<Vec3i32> faces = {
        {0, 2, 1}, {0, 3, 2},
        {4, 5, 6}, {4, 6, 7},
        {0, 1, 5}, {0, 5, 4},
        {1, 2, 6}, {1, 6, 5},
        {2, 3, 7}, {2, 7, 6},
        {3, 0, 4}, {3, 4, 7}
    };
    return TriangleMesh(std::move(vertices), std::move(faces));
}

void rebuild_island_overlap_graph(PrintObject &object);

void run_until_perimeter_input(Orchestrator &orchestrator, Print &print)
{
    Steps::StepLayerHeightGeneration::run_step(orchestrator, print);
    Steps::StepSlicing::run_step(orchestrator, print);

    // Most perimeter tests edit LayerSliceIsland geometry directly after the
    // base cube is sliced. StepPostSlicing finalizes and locks islands, so the
    // mutable fixture attaches regions and rebuilds the upper/lower graph
    // without taking that final production lock.
    for (PrintObject &object : print.objects()) {
        for (Layer &layer : object.layers())
            for (LayerSliceIsland &island : layer.islands())
                island.fill_regions(layer);
        rebuild_island_overlap_graph(object);
    }
}

void set_region_areas(LayerRegion &region, ExPolygons areas)
{
    ExPolygons &region_slices = ApiInternal::LayerRegionAccess::slices_mutable(region);
    region_slices = std::move(areas);
    ApiInternal::LayerRegionAccess::surfaces_mutable(region).set(region_slices, stPosInternal | stDensSolid);
}

void set_region_area(LayerRegion &region, const ExPolygon &area)
{
    set_region_areas(region, ExPolygons{area});
}

void replace_layer_island(Layer &layer, const ExPolygon &area)
{
    ExPolygons islands;
    islands.push_back(area);
    ApiInternal::LayerAccess::set_islands(layer, std::move(islands));
    set_region_area(layer.region(0), area);
    layer.island(0).fill_regions(layer);
}

void replace_layer_islands(Layer &layer, ExPolygons areas)
{
    ApiInternal::LayerAccess::set_islands(layer, ExPolygons(areas));
    set_region_areas(layer.region(0), std::move(areas));
    for (LayerSliceIsland &island : layer.islands())
        island.fill_regions(layer);
}

void rebuild_island_overlap_graph(PrintObject &object)
{
    for (Layer &layer : object.layers())
        for (LayerSliceIsland &island : layer.islands()) {
            island.overlaps_above.clear();
            island.overlaps_below.clear();
        }

    for (size_t layer_idx = 1; layer_idx < object.layer_count(); ++layer_idx)
        Layer::build_up_down_graph(object.layer(layer_idx - 1), object.layer(layer_idx));
}

void add_partitioned_region(PreparedPerimeterPrint &prepared,
                            Layer &layer,
                            const ExPolygon &area,
                            const std::vector<std::pair<std::string, std::string>> &settings)
{
    LayerRegion &default_region = layer.region(0);
    const ExPolygons default_areas = ApiInternal::LayerRegionAccess::slices_mutable(default_region);
    ExPolygons override_areas = intersection_ex(default_areas, area);
    assert(!override_areas.empty());
    if (override_areas.empty())
        return;

    set_region_areas(default_region, diff_ex(default_areas, area));

    PrintRegionConfig config = layer.region(0).region().config();
    for (const std::pair<std::string, std::string> &entry : settings)
        config.set_deserialize_strict(entry.first, entry.second);
    prepared.extra_regions.push_back(std::make_unique<PrintRegion>(config));
    ApiInternal::LayerAccess::add_region(layer, *prepared.extra_regions.back());

    LayerRegion &region = layer.region(layer.region_count() - 1);
    set_region_areas(region, std::move(override_areas));
    layer.island(0).fill_regions(layer);
}

void add_partitioned_region(PreparedPerimeterPrint &prepared,
                            Layer &layer,
                            const ExPolygon &area,
                            const std::string &key,
                            const std::string &value)
{
    add_partitioned_region(prepared, layer, area, std::vector<std::pair<std::string, std::string>>{{key, value}});
}

void paint_generic_facets(ModelVolume &volume,
                          const std::string &key,
                          const EnforcerBlockerType type,
                          const std::vector<int> &facets)
{
    const char *encoded_state = type == EnforcerBlockerType::ENFORCER ? "4" :
                                type == EnforcerBlockerType::BLOCKER  ? "8" : "0";
    FacetsAnnotation &annotation = volume.facets_annotation_mutable(key);
    annotation.reserve(int(facets.size()));
    for (int facet_idx : facets)
        annotation.set_triangle_from_string(facet_idx, encoded_state);
    annotation.shrink_to_fit();
}

void apply_generic_facet_paintings(PrintObject &object,
                                   const std::vector<GenericFacetPaintingOverride> &paintings)
{
    ModelObject *model_object = object.model_object();
    REQUIRE(model_object != nullptr);
    REQUIRE_FALSE(model_object->volumes.empty());
    ModelVolume &model_volume = *model_object->volumes.front();
    for (const GenericFacetPaintingOverride &painting : paintings)
        paint_generic_facets(model_volume, painting.key, painting.type, painting.facets);
}

PerimeterRunCapture capture_perimeter_outputs(const LayerSliceIsland &island)
{
    PerimeterRunCapture capture;
    REQUIRE_FALSE(island.regions().empty());
    const LayerRegion &first_region = **island.regions().begin();
    const Flow external_flow = first_region.flow(frExternalPerimeter);
    capture.external_perimeter_width = external_flow.scaled_width();
    capture.external_perimeter_spacing = external_flow.scaled_spacing();
    capture.external_perimeter_mm3_per_mm = external_flow.mm3_per_mm();
    capture.external_perimeter_width_mm = external_flow.width();
    capture.external_perimeter_height_mm = external_flow.height();

    bool has_external_perimeters = false;
    for (const LayerRegionIsland &region_island : island.regions_islands())
        if (region_island.has_extrusion(LayerRegionIsland::PERIMETERS)) {
            if (!has_external_perimeters) {
                capture.external_perimeters = region_island.extrusion(LayerRegionIsland::PERIMETERS);
                has_external_perimeters = true;
            } else {
                capture.external_perimeters.append(region_island.extrusion(LayerRegionIsland::PERIMETERS));
            }
        }
    capture.fill_surfaces.set(island.infill_areas(), stPosInternal | stDensSolid);
    capture.fill_no_overlap_surfaces.set(island.infill_free_areas(), stPosInternal | stDensSolid);
    return capture;
}

PerimeterRunCapture run_active_perimeter_plugins(Print &print, LayerSliceIsland &island)
{
    Orchestrator &orchestrator = Orchestrator::instance();
    Steps::StepGeneratePerimeter::clean_and_prepare(print);
    Steps::StepGeneratePerimeter::run_step(orchestrator, print);
    return capture_perimeter_outputs(island);
}

size_t count_leaf_extrusions(const ExtrusionEntity &entity)
{
    if (entity.is_nop())
        return 0;
    if (entity.is_leaf())
        return entity.has_polyline() ? 1 : 0;

    size_t count = 0;
    for (size_t child_idx = 0; child_idx < entity.child_count(); ++child_idx)
        count += count_leaf_extrusions(entity.child(child_idx));
    return count;
}

size_t count_direct_default_perimeter_loops(const ExtrusionEntity &entity)
{
    if (entity.is_nop())
        return 0;

    if (entity.is_loop()) {
        const ExtrusionPropertyLoopRole *loop_role = entity.get_property<ExtrusionPropertyLoopRole>();
        if (loop_role == nullptr)
            return 1;
        // In the plugin ABI, LOOP is the base flag and HOLE is a modifier.
        // Contour loops are therefore "LOOP without HOLE", not every entity
        // that merely has the LOOP bit set.
        const ExtrusionLoopRole flags = loop_role->perimeter_role();
        return (flags & elrDefault) != 0 && (flags & elrHole) == 0 ? 1 : 0;
    }

    size_t count = 0;
    for (size_t child_idx = 0; child_idx < entity.child_count(); ++child_idx)
        count += count_direct_default_perimeter_loops(entity.child(child_idx));
    return count;
}

void count_vertical_split_leaf_extrusions(const ExtrusionEntity &entity,
                                          coord_t split_x,
                                          VerticalSplitCounts &out)
{
    if (entity.is_nop())
        return;

    if (entity.is_leaf()) {
        Points points;
        entity.collect_points(points);
        if (points.empty())
            return;

        bool has_left = false;
        bool has_right = false;
        for (const Point &point : points) {
            has_left |= point.x() < split_x;
            has_right |= point.x() > split_x;
        }

        if (has_left && !has_right)
            ++out.left_only;
        else if (has_right && !has_left)
            ++out.right_only;
        else
            ++out.crossing;
        return;
    }

    for (size_t child_idx = 0; child_idx < entity.child_count(); ++child_idx)
        count_vertical_split_leaf_extrusions(entity.child(child_idx), split_x, out);
}

struct TestPerimeterNode
{
    TestPerimeterNode *parent = nullptr;
    ExPolygon area;
    ExPolygon fill_area;
    ExtrusionEntityCollection extrusions;
    std::vector<std::unique_ptr<TestPerimeterNode>> children;
    std::vector<perimeter_node *> c_children;
    perimeter_node c_node = {};

    void refresh_c_node()
    {
        c_children.clear();
        c_children.reserve(children.size());
        for (std::unique_ptr<TestPerimeterNode> &child : children) {
            child->parent = this;
            child->refresh_c_node();
            c_children.push_back(&child->c_node);
        }

        c_node.parent = parent == nullptr ? nullptr : &parent->c_node;
        c_node.area = reinterpret_cast<expolygon_handle *>(&area);
        c_node.fill_area = reinterpret_cast<expolygon_handle *>(&fill_area);
        c_node.extrusions = reinterpret_cast<extrusion_entity_handle *>(&extrusions);
        c_node.children = c_children.empty() ? nullptr : c_children.data();
        c_node.child_count = uint32_t(c_children.size());
    }
};

TestPerimeterNode *find_test_node(TestPerimeterNode &node, perimeter_node *c_node)
{
    if (&node.c_node == c_node)
        return &node;
    for (std::unique_ptr<TestPerimeterNode> &child : node.children) {
        TestPerimeterNode *found = find_test_node(*child, c_node);
        if (found != nullptr)
            return found;
    }
    return nullptr;
}

void test_split_node(perimeter_generation_context *context,
                     perimeter_node *node,
                     const expolygon_collection_handle *clip_handle,
                     perimeter_node_span *inside_nodes_out)
{
    REQUIRE(context != nullptr);
    REQUIRE(context->generator_context != nullptr);
    REQUIRE(inside_nodes_out != nullptr);
    TestPerimeterNode &root = *reinterpret_cast<TestPerimeterNode *>(context->generator_context);
    TestPerimeterNode *test_node = find_test_node(root, node);
    REQUIRE(test_node != nullptr);

    if (clip_handle == nullptr) {
        root.c_children = { &test_node->c_node };
        inside_nodes_out->items = root.c_children.data();
        inside_nodes_out->count = 1;
        return;
    }

    const ExPolygons &clip = *reinterpret_cast<const ExPolygons *>(clip_handle);
    ExPolygons inside = intersection_ex(ExPolygons{test_node->area}, clip);
    ExPolygons outside = diff_ex(ExPolygons{test_node->area}, clip);

    test_node->children.clear();
    root.c_children.clear();
    for (ExPolygon &area : inside) {
        std::unique_ptr<TestPerimeterNode> child(new TestPerimeterNode);
        child->area = std::move(area);
        child->fill_area = child->area;
        child->c_node.perimeter_idx = node->perimeter_idx;
        child->c_node.perimeter_needed = node->perimeter_needed;
        test_node->children.push_back(std::move(child));
        root.c_children.push_back(&test_node->children.back()->c_node);
    }
    for (ExPolygon &area : outside) {
        std::unique_ptr<TestPerimeterNode> child(new TestPerimeterNode);
        child->area = std::move(area);
        child->fill_area = child->area;
        child->c_node.perimeter_idx = node->perimeter_idx;
        child->c_node.perimeter_needed = node->perimeter_needed;
        test_node->children.push_back(std::move(child));
    }
    root.refresh_c_node();

    inside_nodes_out->items = root.c_children.empty() ? nullptr : root.c_children.data();
    inside_nodes_out->count = uint32_t(root.c_children.size());
}

ExtrusionPath open_gap_fill_path()
{
    ExtrusionPath path(ExtrusionAttributes(ExtrusionRole::GapFill, ExtrusionFlow(0.1, 0.4f, 0.2f)), nullptr, true);
    path.polyline().append(Point(scale_i(-8.), 0));
    path.polyline().append(Point(scale_i(8.), 0));
    return path;
}

ExtrusionPath closed_perimeter_path(const double min_x, const double min_y, const double max_x, const double max_y)
{
    ExtrusionPath path(ExtrusionAttributes(ExtrusionRole::ExternalPerimeter, ExtrusionFlow(0.1, 0.4f, 0.2f)), nullptr, true);
    path.polyline().append(Point(scale_i(min_x), scale_i(min_y)));
    path.polyline().append(Point(scale_i(max_x), scale_i(min_y)));
    path.polyline().append(Point(scale_i(max_x), scale_i(max_y)));
    path.polyline().append(Point(scale_i(min_x), scale_i(max_y)));
    path.polyline().append(Point(scale_i(min_x), scale_i(min_y)));
    return path;
}

ExtrusionLoop perimeter_loop(const ExtrusionLoopRole role, const double inset)
{
    return ExtrusionLoop(closed_perimeter_path(-8. + inset, -8. + inset, 8. - inset, 8. - inset), role);
}

double area_sum(const ExPolygons &areas)
{
    double out = 0.;
    for (const ExPolygon &area : areas)
        out += std::abs(area.area());
    return out;
}

ExPolygons surface_expolygons(const SurfaceCollection &surfaces)
{
    ExPolygons out;
    out.reserve(surfaces.size());
    for (const Surface &surface : surfaces)
        if (!surface.empty())
            out.push_back(surface.expolygon);
    return out;
}

double area_tolerance()
{
    const double side = double(scale_i(0.005));
    return side * side;
}

void require_no_positive_overlap(const ExPolygons &areas)
{
    for (size_t lhs_idx = 0; lhs_idx < areas.size(); ++lhs_idx)
        for (size_t rhs_idx = lhs_idx + 1; rhs_idx < areas.size(); ++rhs_idx) {
            INFO("overlap between generated leaf areas " << lhs_idx << " and " << rhs_idx);
            REQUIRE(area_sum(intersection_ex(areas[lhs_idx], areas[rhs_idx])) <= area_tolerance());
        }
}

void require_same_union(const ExPolygons &actual, const ExPolygons &expected)
{
    const ExPolygons actual_union = union_ex(actual);
    const ExPolygons expected_union = union_ex(expected);
    INFO("actual area " << area_sum(actual_union) << ", expected area " << area_sum(expected_union));
    REQUIRE(area_sum(diff_ex(actual_union, expected_union)) <= area_tolerance());
    REQUIRE(area_sum(diff_ex(expected_union, actual_union)) <= area_tolerance());
}

perimeter_generation_module_instance create_module_instance(const char *plugin_id,
                                                            Print &print,
                                                            plugin_run_context &run_context,
                                                            plugin_host_context &host_context,
                                                            run_ctx_perimeter_generation_module &payload)
{
    Orchestrator &orchestrator = Orchestrator::instance();
    Plugin *plugin = orchestrator.get_plugin(plugin_id);
    REQUIRE(plugin != nullptr);
    host_context = orchestrator.prepare_plugin_host_context(PERIMETER_GENERATION_MODULE, plugin, &print);
    run_context = orchestrator.prepare_plugin_run_context(PERIMETER_GENERATION_MODULE, plugin, &host_context);
    payload = {};
    run_context.data = &payload;
    plugin->setup(run_context, 1);
    plugin->setup_run(run_context);
    plugin->run(run_context);
    REQUIRE(payload.module.vt != nullptr);
    return payload.module;
}

} // namespace

const char *const SIMPLE_PERIMETER_GENERATOR = "perimeter.generator.simple";
const char *const PYTHON_SIMPLE_PERIMETER_GENERATOR = "python.perimeter.generator.simple";
const char *const ARACHNE_PERIMETER_GENERATOR = "perimeter.generator.arachne";
const char *const CLASSIC_PERIMETER_GENERATOR = "perimeter.generator.classic";
const char *const DENSE_INFILL_SURFACE_MARKER = "dense_infill.surface_marker";
const char *const DENSE_INFILL_RECIPE_MODIFIER = "dense_infill.recipe_modifier";
const char *const DENSE_INFILL_POST_INFILL_ORDER = "dense_infill.post_infill_order";
const char *const EXTRA_PERIMETER_COUNT = "perimeter.module.extra_perimeter_count";
const char *const EXTRA_PERIMETER_BELOW_AREA = "perimeter.module.extra_perimeter_below_area";
const char *const EXTRA_PERIMETER_ODD_LAYER = "perimeter.module.extra_perimeter_odd_layer";
const char *const ONLY_ONE_PERIMETER_FIRST_LAYER = "perimeter.module.only_one_perimeter_first_layer";
const char *const ONLY_ONE_PERIMETER_ON_TOP = "perimeter.module.only_one_perimeter_on_top";
const char *const SEPARATE_HOLE_CONTOUR = "perimeter.module.separate_hole_contour";
const char *const REMOVE_GAP_FILL_ON_OVERHANGS = "perimeter.module.remove_gap_fill_on_overhangs";
const char *const MARK_FIRST_LOOP = "perimeter.module.mark_first_loop";
const char *const EXTRA_PERIMETERS_ON_OVERHANGS = "perimeter.post_process.extra_perimeters_on_overhangs";
const char *const EXTRA_PERIMETER_OVERHANG_WAVE = "perimeter.post_process.extra_perimeter_overhang_wave";
const char *const DETECT_OVERHANG = "perimeter.post_process.detect_overhang";
const char *const FUZZY_SKIN = "perimeter.post_process.fuzzy_skin";
const char *const INITIAL_TYPED_SURFACE_BUILDER = "surface.initial_typed_surface_builder";
const char *const SOLID_SHELLS = "surface.solid_shells";
const char *const TOP_SURFACE_EXPANSION = "surface.top_surface_expansion";
const char *const CLEAN_INFILL_SURFACES = "surface.clean_infill_surfaces";
const char *const INFILL_REGION_COMPATIBILITY_SPLITTER = "surface.infill_region_compatibility_splitter";

ExPolygon rectangle_expolygon(const double min_x, const double min_y, const double max_x, const double max_y)
{
    return ExPolygon(Polygon({
        Point(scale_i(min_x), scale_i(min_y)),
        Point(scale_i(max_x), scale_i(min_y)),
        Point(scale_i(max_x), scale_i(max_y)),
        Point(scale_i(min_x), scale_i(max_y))
    }));
}

ExPolygon rectangle_with_hole_expolygon()
{
    ExPolygon out = rectangle_expolygon(-10., -10., 10., 10.);
    out.holes.push_back(Polygon({
        Point(scale_i(-3.), scale_i(-3.)),
        Point(scale_i(-3.), scale_i(3.)),
        Point(scale_i(3.), scale_i(3.)),
        Point(scale_i(3.), scale_i(-3.))
    }));
    return out;
}

DynamicPrintConfig perimeter_config(std::initializer_list<std::pair<std::string, std::string>> overrides)
{
    Slic3r::Test::Plugins::ensure_plugin_test_runtime_initialized();

    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({
        {"layer_height", "1"},
        {"first_layer_height", "1"},
        {"nozzle_diameter", "0.4"},
        {"perimeters", "1"},
        {"extra_perimeters_count", "0"},
        {"extra_perimeters_below_area", "0"},
        {"extra_perimeters_odd_layers", "0"},
        {"only_one_perimeter_first_layer", "0"},
        {"only_one_perimeter_top", "0"},
        {"perimeters_hole", "!0"},
        {"gap_fill_no_overhang", "0"},
        {"gap_fill_enabled", "1"},
        {"fill_density", "15%"}
    });
    for (const std::pair<std::string, std::string> &entry : overrides)
        config.set_deserialize_strict(entry.first, entry.second);
    return config;
}

void prepare_cube_print(PreparedPerimeterPrint &prepared, const DynamicPrintConfig &config)
{
    const TriangleMesh cube = make_box({-10., -10., 0., 10., 10., 10.});
    Slic3r::Test::init_print({cube}, prepared.print, prepared.model, config);
    run_until_perimeter_input(Orchestrator::instance(), prepared.print);
}

size_t layer_index_for_top(const PrintObject &object)
{
    REQUIRE(object.layer_count() > 0);
    return object.layer_count() - 1;
}

size_t layer_index_for_odd_layer(const PrintObject &object)
{
    REQUIRE(object.layer_count() > 1);
    return 1;
}

PerimeterRunCapture run_perimeter_case(
    const DynamicPrintConfig &config,
    std::initializer_list<const char *> active_plugins,
    const ExPolygon &area,
    const size_t layer_idx,
    std::initializer_list<std::pair<std::string, std::string>> region_overrides,
    const ExPolygon *region_area)
{
    PreparedPerimeterPrint prepared;
    prepare_cube_print(prepared, config);
    PrintObject &object = prepared.print.object(0);
    REQUIRE(layer_idx < object.layer_count());
    Layer &layer = object.layer(layer_idx);
    replace_layer_island(layer, area);
    rebuild_island_overlap_graph(object);

    if (region_overrides.size() > 0) {
        const ExPolygon default_region_area = rectangle_expolygon(-6., -6., 6., 6.);
        const ExPolygon &override_area = region_area != nullptr ? *region_area : default_region_area;
        for (const std::pair<std::string, std::string> &entry : region_overrides)
            add_partitioned_region(prepared, layer, override_area, entry.first, entry.second);
    }

    ScopedActivePlugins active_scope(active_plugins);
    return run_active_perimeter_plugins(prepared.print, layer.island(0));
}

PerimeterRunCapture run_perimeter_and_post_case(
    const DynamicPrintConfig &config,
    std::initializer_list<const char *> perimeter_plugins,
    std::initializer_list<const char *> post_plugins,
    const ExPolygon &area,
    const size_t layer_idx,
    std::initializer_list<std::pair<std::string, std::string>> region_overrides,
    const ExPolygon *region_area)
{
    PreparedPerimeterPrint prepared;
    prepare_cube_print(prepared, config);
    PrintObject &object = prepared.print.object(0);
    REQUIRE(layer_idx < object.layer_count());
    Layer &layer = object.layer(layer_idx);
    replace_layer_island(layer, area);
    rebuild_island_overlap_graph(object);

    if (region_overrides.size() > 0) {
        const ExPolygon default_region_area = rectangle_expolygon(-6., -6., 6., 6.);
        const ExPolygon &override_area = region_area != nullptr ? *region_area : default_region_area;
        for (const std::pair<std::string, std::string> &entry : region_overrides)
            add_partitioned_region(prepared, layer, override_area, entry.first, entry.second);
    }

    {
        ScopedActivePlugins active_scope(perimeter_plugins);
        Steps::StepGeneratePerimeter::clean_and_prepare(prepared.print);
        Steps::StepGeneratePerimeter::run_step(Orchestrator::instance(), prepared.print);
    }

    {
        ScopedActivePlugins active_scope(post_plugins);
        Steps::StepPostPerimeterGeneration::run_step(Orchestrator::instance(), prepared.print);
    }

    return capture_perimeter_outputs(layer.island(0));
}

PerimeterRunCapture run_perimeter_and_post_case_with_regions(
    const DynamicPrintConfig &config,
    std::initializer_list<const char *> perimeter_plugins,
    std::initializer_list<const char *> post_plugins,
    const ExPolygon &area,
    const size_t layer_idx,
    const std::vector<PerimeterRegionOverride> &region_overrides)
{
    PreparedPerimeterPrint prepared;
    prepare_cube_print(prepared, config);
    PrintObject &object = prepared.print.object(0);
    REQUIRE(layer_idx < object.layer_count());
    Layer &layer = object.layer(layer_idx);
    replace_layer_island(layer, area);
    rebuild_island_overlap_graph(object);

    for (const PerimeterRegionOverride &override_region : region_overrides)
        add_partitioned_region(prepared, layer, override_region.area, override_region.settings);

    {
        ScopedActivePlugins active_scope(perimeter_plugins);
        Steps::StepGeneratePerimeter::clean_and_prepare(prepared.print);
        Steps::StepGeneratePerimeter::run_step(Orchestrator::instance(), prepared.print);
    }

    {
        ScopedActivePlugins active_scope(post_plugins);
        Steps::StepPostPerimeterGeneration::run_step(Orchestrator::instance(), prepared.print);
    }

    return capture_perimeter_outputs(layer.island(0));
}

PerimeterRunCapture run_perimeter_and_post_case_with_lower_area(
    const DynamicPrintConfig &config,
    std::initializer_list<const char *> perimeter_plugins,
    std::initializer_list<const char *> post_plugins,
    const ExPolygon &area,
    const ExPolygon &lower_area,
    const size_t layer_idx,
    std::initializer_list<std::pair<std::string, std::string>> region_overrides,
    const ExPolygon *region_area)
{
    PreparedPerimeterPrint prepared;
    prepare_cube_print(prepared, config);
    PrintObject &object = prepared.print.object(0);
    REQUIRE(layer_idx > 0);
    REQUIRE(layer_idx < object.layer_count());

    // Overhang post-process tests need a target island and an independently
    // shaped island below it. The normal fixture slices a straight cube, so we
    // patch both layers and rebuild the upper/lower links before running the
    // perimeter steps.
    Layer &lower_layer = object.layer(layer_idx - 1);
    replace_layer_island(lower_layer, lower_area);
    Layer &layer = object.layer(layer_idx);
    replace_layer_island(layer, area);
    rebuild_island_overlap_graph(object);

    if (region_overrides.size() > 0) {
        const ExPolygon default_region_area = rectangle_expolygon(-6., -6., 6., 6.);
        const ExPolygon &override_area = region_area != nullptr ? *region_area : default_region_area;
        for (const std::pair<std::string, std::string> &entry : region_overrides)
            add_partitioned_region(prepared, layer, override_area, entry.first, entry.second);
    }

    {
        ScopedActivePlugins active_scope(perimeter_plugins);
        Steps::StepGeneratePerimeter::clean_and_prepare(prepared.print);
        Steps::StepGeneratePerimeter::run_step(Orchestrator::instance(), prepared.print);
    }

    {
        ScopedActivePlugins active_scope(post_plugins);
        Steps::StepPostPerimeterGeneration::run_step(Orchestrator::instance(), prepared.print);
    }

    return capture_perimeter_outputs(layer.island(0));
}

PerimeterRunCapture run_perimeter_and_post_case_with_generic_facet_painting(
    const DynamicPrintConfig &config,
    std::initializer_list<const char *> perimeter_plugins,
    std::initializer_list<const char *> post_plugins,
    const ExPolygon &area,
    const size_t layer_idx,
    const std::vector<GenericFacetPaintingOverride> &paintings)
{
    PreparedPerimeterPrint prepared;
    prepare_cube_print(prepared, config);
    PrintObject &object = prepared.print.object(0);
    REQUIRE(layer_idx < object.layer_count());
    apply_generic_facet_paintings(object, paintings);

    Layer &layer = object.layer(layer_idx);
    replace_layer_island(layer, area);
    rebuild_island_overlap_graph(object);

    {
        ScopedActivePlugins active_scope(perimeter_plugins);
        Steps::StepGeneratePerimeter::clean_and_prepare(prepared.print);
        Steps::StepGeneratePerimeter::run_step(Orchestrator::instance(), prepared.print);
    }

    {
        ScopedActivePlugins active_scope(post_plugins);
        Steps::StepPostPerimeterGeneration::run_step(Orchestrator::instance(), prepared.print);
    }

    return capture_perimeter_outputs(layer.island(0));
}

PerimeterMultiIslandRunCapture run_perimeter_multi_island_case(
    const DynamicPrintConfig &config,
    std::initializer_list<const char *> active_plugins,
    const ExPolygons &areas,
    const size_t layer_idx)
{
    PreparedPerimeterPrint prepared;
    prepare_cube_print(prepared, config);
    PrintObject &object = prepared.print.object(0);
    REQUIRE(layer_idx < object.layer_count());
    Layer &layer = object.layer(layer_idx);
    replace_layer_islands(layer, areas);
    rebuild_island_overlap_graph(object);

    ScopedActivePlugins active_scope(active_plugins);
    Orchestrator &orchestrator = Orchestrator::instance();
    Steps::StepGeneratePerimeter::clean_and_prepare(prepared.print);
    Steps::StepGeneratePerimeter::run_step(orchestrator, prepared.print);

    PerimeterMultiIslandRunCapture out;
    out.islands.reserve(layer.islands().size());
    for (const LayerSliceIsland &island : layer.islands())
        out.islands.push_back(capture_perimeter_outputs(island));
    return out;
}

size_t external_perimeter_count(const PerimeterRunCapture &capture)
{
    return count_leaf_extrusions(capture.external_perimeters);
}

const ExtrusionEntityCollection &external_perimeters(const PerimeterRunCapture &capture)
{
    return capture.external_perimeters;
}

double extrusion_length(const ExtrusionEntity &entity)
{
    if (entity.is_nop())
        return 0.;
    if (entity.is_leaf())
        return entity.has_polyline() ? entity.length() : 0.;

    double length = 0.;
    for (size_t child_idx = 0; child_idx < entity.child_count(); ++child_idx)
        length += extrusion_length(entity.child(child_idx));
    return length;
}

size_t total_polyline_points(const ExtrusionEntity &entity)
{
    if (entity.is_nop())
        return 0;
    if (entity.is_leaf()) {
        Points points;
        entity.collect_points(points);
        return points.size();
    }

    size_t count = 0;
    for (size_t child_idx = 0; child_idx < entity.child_count(); ++child_idx)
        count += total_polyline_points(entity.child(child_idx));
    return count;
}

size_t count_loops_with_role(const ExtrusionEntity &entity, const ExtrusionLoopRole role_mask)
{
    if (const ExtrusionPropertyLoopRole *perimeter = entity.get_property<ExtrusionPropertyLoopRole>()) {
        const ExtrusionLoopRole flags = perimeter->perimeter_role();
        // Most tests pass elrDefault when they mean "contour loop". The current
        // perimeter flag model uses elrDefault/C_EXTRUSION_PERIMETER_FLAG_LOOP
        // as the base loop marker, so holes also carry it and must be excluded.
        if (role_mask == elrDefault)
            return (flags & elrDefault) != 0 && (flags & elrHole) == 0 ? 1 : 0;
        return (flags & role_mask) != 0 ? 1 : 0;
    }

    size_t count = 0;
    for (size_t child_idx = 0; child_idx < entity.child_count(); ++child_idx)
        count += count_loops_with_role(entity.child(child_idx), role_mask);
    return count;
}

VerticalSplitCounts vertical_split_counts(const ExtrusionEntity &entity, coord_t split_x)
{
    VerticalSplitCounts out;
    count_vertical_split_leaf_extrusions(entity, split_x, out);
    return out;
}

void require_leaf_fill_area_consistency(const PerimeterRunCapture &capture)
{
    const ExPolygons leaf_areas = surface_expolygons(capture.fill_no_overlap_surfaces);
    const ExPolygons leaf_fill_areas = surface_expolygons(capture.fill_surfaces);

    // A generated perimeter can consume a very thin branch completely. In that
    // case there is no remaining infill job to validate; the important part is
    // that both published leaf domains agree that no fill is left.
    if (leaf_areas.empty() || leaf_fill_areas.empty()) {
        REQUIRE(leaf_areas.empty());
        REQUIRE(leaf_fill_areas.empty());
        REQUIRE(external_perimeter_count(capture) > 0);
        return;
    }

    // Perimeter modules may split or rebuild the tree, but final leaf areas
    // must remain a clean partition. Infill later consumes these leaves as
    // independent jobs, so any positive overlap here would double-process area.
    require_no_positive_overlap(leaf_areas);
    require_no_positive_overlap(leaf_fill_areas);

    // Fill areas are the anchoring domains attached to the same leaves. They
    // may be larger than the strict no-overlap areas, but they must never miss
    // any part of those strict areas.
    const ExPolygons leaf_area_union = union_ex(leaf_areas);
    const ExPolygons leaf_fill_union = union_ex(leaf_fill_areas);
    REQUIRE(area_sum(diff_ex(leaf_area_union, leaf_fill_union)) <= area_tolerance());

    const double leaf_area = area_sum(leaf_area_union);
    const double leaf_fill_area = area_sum(leaf_fill_union);
    REQUIRE(leaf_fill_area + area_tolerance() >= leaf_area);
}

void require_simple_generator_first_child_area_partition(const PerimeterRunCapture &capture, const ExPolygon &parent_area)
{
    require_leaf_fill_area_consistency(capture);

    const ExPolygons leaf_areas = surface_expolygons(capture.fill_no_overlap_surfaces);
    const ExPolygons leaf_fill_areas = surface_expolygons(capture.fill_surfaces);

    // For the simple generator after the first external perimeter, children are
    // created from the parent area shrunk past the external wall and half the
    // next perimeter spacing. Modules may split those children, but their union
    // must still be exactly the generated child domain.
    const double leaf_delta =
        -0.5 * double(capture.external_perimeter_width + capture.external_perimeter_spacing);
    require_same_union(leaf_areas, offset_ex(parent_area, leaf_delta));

    // The fill/anchor domain follows the same split, but is 25% of perimeter
    // spacing larger than the strict child area.
    const double fill_delta = leaf_delta + 0.25 * double(capture.external_perimeter_spacing);
    require_same_union(leaf_fill_areas, offset_ex(parent_area, fill_delta));
}

size_t run_remove_gap_fill_module(const DynamicPrintConfig &config,
                                  const bool use_region_override,
                                  double *length_out,
                                  const size_t layer_idx)
{
    PreparedPerimeterPrint prepared;
    prepare_cube_print(prepared, config);
    PrintObject &object = prepared.print.object(0);
    REQUIRE(layer_idx < object.layer_count());
    Layer &layer = object.layer(layer_idx);
    const ExPolygon area = rectangle_expolygon(-10., -10., 10., 10.);
    replace_layer_island(layer, area);
    rebuild_island_overlap_graph(object);
    if (use_region_override)
        add_partitioned_region(prepared, layer, rectangle_expolygon(-1., -10., 10., 10.), "gap_fill_no_overhang", "1");

    TestPerimeterNode root;
    root.area = area;
    root.fill_area = area;
    root.extrusions.append(open_gap_fill_path());
    root.c_node.perimeter_idx = 0;
    root.c_node.perimeter_needed = 1;
    root.refresh_c_node();

    plugin_host_context host_context = {};
    plugin_run_context run_context = {};
    run_ctx_perimeter_generation_module payload = {};
    perimeter_generation_module_instance module =
        create_module_instance(REMOVE_GAP_FILL_ON_OVERHANGS, prepared.print, run_context, host_context, payload);

    perimeter_generation_context context = {};
    context.run_ctx = &run_context;
    context.print = reinterpret_cast<const print_handle *>(&prepared.print);
    context.object = reinterpret_cast<const object_handle *>(&object);
    context.layer = reinterpret_cast<const layer_handle *>(&layer);
    context.island = reinterpret_cast<const layer_island_handle *>(&layer.island(0));
    context.root = &root.c_node;
    context.generator_context = &root;
    context.split_node = &test_split_node;

    REQUIRE(module.vt->start != nullptr);
    REQUIRE(module.vt->after != nullptr);
    REQUIRE(module.vt->end != nullptr);
    void *module_context = module.vt->start(module.ctx, &context);
    module.vt->after(module.ctx, module_context, &context, &root.c_node);
    module.vt->end(module.ctx, module_context, &context);
    if (length_out != nullptr)
        *length_out = extrusion_length(root.extrusions);
    return count_leaf_extrusions(root.extrusions);
}

SeparateHoleContourDirectResult run_separate_hole_contour_module_direct(
    const DynamicPrintConfig &config,
    const uint32_t perimeter_idx,
    const uint32_t perimeter_needed,
    const uint32_t contour_loop_count,
    const uint32_t hole_loop_count,
    const bool add_open_polyline,
    const bool add_unclassified_closed_loop)
{
    PreparedPerimeterPrint prepared;
    prepare_cube_print(prepared, config);
    PrintObject &object = prepared.print.object(0);
    Layer &layer = object.layer(0);
    const ExPolygon area = rectangle_with_hole_expolygon();
    replace_layer_island(layer, area);
    rebuild_island_overlap_graph(object);

    TestPerimeterNode root;
    root.area = area;
    root.fill_area = area;
    root.c_node.perimeter_idx = perimeter_idx;
    root.c_node.perimeter_needed = perimeter_needed;

    for (uint32_t idx = 0; idx < contour_loop_count; ++idx)
        root.extrusions.append(perimeter_loop(elrDefault, double(idx) * 0.5));
    for (uint32_t idx = 0; idx < hole_loop_count; ++idx)
        root.extrusions.append(perimeter_loop(ExtrusionLoopRole(elrDefault | elrHole), double(idx) * 0.5));
    if (add_open_polyline)
        root.extrusions.append(open_gap_fill_path());
    if (add_unclassified_closed_loop)
        root.extrusions.append(closed_perimeter_path(-5., -5., 5., 5.));
    root.refresh_c_node();

    plugin_host_context host_context = {};
    plugin_run_context run_context = {};
    run_ctx_perimeter_generation_module payload = {};
    perimeter_generation_module_instance module =
        create_module_instance(SEPARATE_HOLE_CONTOUR, prepared.print, run_context, host_context, payload);

    perimeter_generation_context context = {};
    context.run_ctx = &run_context;
    context.print = reinterpret_cast<const print_handle *>(&prepared.print);
    context.object = reinterpret_cast<const object_handle *>(&object);
    context.layer = reinterpret_cast<const layer_handle *>(&layer);
    context.island = reinterpret_cast<const layer_island_handle *>(&layer.island(0));
    context.root = &root.c_node;
    context.generator_context = &root;
    context.split_node = &test_split_node;

    REQUIRE(module.vt->start != nullptr);
    REQUIRE(module.vt->after != nullptr);
    REQUIRE(module.vt->end != nullptr);
    void *module_context = module.vt->start(module.ctx, &context);
    module.vt->after(module.ctx, module_context, &context, &root.c_node);
    module.vt->end(module.ctx, module_context, &context);

    SeparateHoleContourDirectResult result;
    result.total = count_leaf_extrusions(root.extrusions);
    result.contours = count_direct_default_perimeter_loops(root.extrusions);
    result.holes = count_loops_with_role(root.extrusions, elrHole);
    result.children = root.children.size();
    result.perimeter_needed = root.c_node.perimeter_needed;
    return result;
}

} // namespace Slic3r::Test::PerimeterPluginTests
