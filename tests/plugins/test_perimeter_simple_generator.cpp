#include <catch2/catch.hpp>

#include "perimeter_test_helpers.hpp"

#include <vector>

namespace {
using namespace Slic3r;
using namespace Slic3r::Test::PerimeterPluginTests;

struct SimpleLoopInfo
{
    const ExtrusionEntity *loop = nullptr;
    const ExtrusionEntity *path = nullptr;
    const ExtrusionPropertyLoopRole *perimeter = nullptr;
    const ExtrusionAttributes *attributes = nullptr;
};

Polygon clockwise_hole(const double min_x, const double min_y, const double max_x, const double max_y)
{
    Polygon hole({
        Point(scale_i(min_x), scale_i(min_y)),
        Point(scale_i(min_x), scale_i(max_y)),
        Point(scale_i(max_x), scale_i(max_y)),
        Point(scale_i(max_x), scale_i(min_y))
    });
    hole.make_clockwise();
    return hole;
}

ExPolygon rectangle_with_hole(const double hole_min_x,
                              const double hole_min_y,
                              const double hole_max_x,
                              const double hole_max_y)
{
    ExPolygon out = rectangle_expolygon(-10., -10., 10., 10.);
    out.holes.push_back(clockwise_hole(hole_min_x, hole_min_y, hole_max_x, hole_max_y));
    return out;
}

ExPolygon close_to_edge_hole()
{
    return rectangle_with_hole(7.4, -3.5, 9.3, 3.5);
}

void collect_simple_loops(const ExtrusionEntity &entity, std::vector<SimpleLoopInfo> &out)
{
    const ExtrusionPropertyLoopRole *perimeter = entity.get_property<ExtrusionPropertyLoopRole>();
    if (perimeter != nullptr) {
        SimpleLoopInfo info;
        info.loop = &entity;
        info.perimeter = perimeter;
        for (size_t child_idx = 0; child_idx < entity.child_count(); ++child_idx) {
            const ExtrusionEntity &child = entity.child(child_idx);
            if (child.has_polyline()) {
                info.path = &child;
                info.attributes = child.get_property<ExtrusionAttributes>();
                break;
            }
        }
        out.push_back(info);
        return;
    }

    for (size_t child_idx = 0; child_idx < entity.child_count(); ++child_idx)
        collect_simple_loops(entity.child(child_idx), out);
}

std::vector<SimpleLoopInfo> simple_loops(const PerimeterRunCapture &capture)
{
    std::vector<SimpleLoopInfo> out;
    collect_simple_loops(capture.external_perimeters, out);
    return out;
}

size_t loop_count_with_role(const std::vector<SimpleLoopInfo> &loops, const ExtrusionLoopRole role)
{
    size_t count = 0;
    for (const SimpleLoopInfo &loop : loops) {
        if (loop.perimeter == nullptr)
            continue;
        const ExtrusionLoopRole flags = loop.perimeter->perimeter_role();
        // The LOOP/default bit now means "this entity is a perimeter loop".
        // A contour is the subset of loops that does not also carry HOLE.
        if (role == elrDefault) {
            if ((flags & elrDefault) != 0 && (flags & elrHole) == 0)
                ++count;
            continue;
        }
        if ((flags & role) != 0)
            ++count;
    }
    return count;
}

size_t loop_count_with_shell(const std::vector<SimpleLoopInfo> &loops, const int16_t perimeter_idx)
{
    size_t count = 0;
    for (const SimpleLoopInfo &loop : loops)
        if (loop.perimeter != nullptr && loop.perimeter->perimeter_idx == perimeter_idx)
            ++count;
    return count;
}

void require_loop_payloads_match_external_flow(const PerimeterRunCapture &capture,
                                               const std::vector<SimpleLoopInfo> &loops)
{
    REQUIRE_FALSE(loops.empty());
    for (const SimpleLoopInfo &loop : loops) {
        REQUIRE(loop.loop != nullptr);
        REQUIRE(loop.path != nullptr);
        REQUIRE(loop.perimeter != nullptr);
        REQUIRE(loop.attributes != nullptr);

        CHECK(loop.loop->is_loop());
        CHECK(loop.loop->is_continuous());
        CHECK(loop.loop->can_reverse());
        CHECK_FALSE(loop.loop->can_sort());
        CHECK(loop.loop->child_count() == 1);

        CHECK(loop.path->has_polyline());
        CHECK(loop.path->is_leaf());
        CHECK(loop.path->role().is_external_perimeter());
        CHECK(loop.attributes->extrusion_role().is_external_perimeter());
        CHECK(loop.attributes->mm3_per_mm == Approx(capture.external_perimeter_mm3_per_mm));
        CHECK(loop.attributes->width == Approx(capture.external_perimeter_width_mm));
        CHECK(loop.attributes->height == Approx(capture.external_perimeter_height_mm));
        CHECK(loop.attributes->no_seam == 0);
    }
}

void require_shell_indices(const std::vector<SimpleLoopInfo> &loops,
                           const size_t expected_shell_count,
                           const size_t loops_per_shell)
{
    for (size_t perimeter_idx = 0; perimeter_idx < expected_shell_count; ++perimeter_idx) {
        INFO("perimeter_idx " << perimeter_idx);
        CHECK(loop_count_with_shell(loops, int16_t(perimeter_idx)) == loops_per_shell);
    }
}

const ExtrusionEntity *single_child_group(const ExtrusionEntity &entity)
{
    const ExtrusionEntity *out = nullptr;
    for (size_t child_idx = 0; child_idx < entity.child_count(); ++child_idx) {
        const ExtrusionEntity &child = entity.child(child_idx);
        if (!child.is_leaf() && child.get_property<ExtrusionPropertyLoopRole>() == nullptr) {
            CHECK(out == nullptr);
            out = &child;
        }
    }
    return out;
}

size_t direct_loop_child_count(const ExtrusionEntity &entity)
{
    size_t count = 0;
    for (size_t child_idx = 0; child_idx < entity.child_count(); ++child_idx)
        if (entity.child(child_idx).get_property<ExtrusionPropertyLoopRole>() != nullptr)
            ++count;
    return count;
}

void require_nested_node_structure(const ExtrusionEntity &node,
                                   const size_t perimeter_idx,
                                   const size_t perimeter_count,
                                   const size_t loops_per_node)
{
    INFO("checking published node level " << perimeter_idx);
    REQUIRE_FALSE(node.is_leaf());
    CHECK(direct_loop_child_count(node) == loops_per_node);

    const ExtrusionEntity *next_node = single_child_group(node);
    if (perimeter_idx + 1 < perimeter_count) {
        REQUIRE(next_node != nullptr);
        require_nested_node_structure(*next_node, perimeter_idx + 1, perimeter_count, loops_per_node);
    } else {
        CHECK(next_node == nullptr);
    }
}

void require_published_tree_matches_perimeter_tree(const PerimeterRunCapture &capture,
                                                   const size_t perimeter_count,
                                                   const size_t loops_per_node)
{
    // The published extrusion tree should mirror the perimeter-node tree: the
    // root collection owns one root node, each node owns the loops generated at
    // that depth, and the next perimeter depth is represented by a child node.
    REQUIRE(capture.external_perimeters.child_count() == 1);
    require_nested_node_structure(capture.external_perimeters.child(0), 0, perimeter_count, loops_per_node);
}
}

TEST_CASE("SimplePerimeterGenerator publishes perimeter and fill output", "[plugins][perimeter]")
{
    // This is the smoke test for the full STEP_PERIMETER payload. With no
    // STEP_PERIMETER plugin active, nothing should be published. With the
    // simple generator active, it should create at least one external loop and
    // both fill-surface collections for the island.
    const DynamicPrintConfig config = perimeter_config({});
    const ExPolygon surface = rectangle_expolygon(-10., -10., 10., 10.);

    const PerimeterRunCapture inactive =
        run_perimeter_case(config, {}, surface, 0);
    REQUIRE(external_perimeter_count(inactive) == 0);

    const PerimeterRunCapture generated =
        run_perimeter_case(config, {SIMPLE_PERIMETER_GENERATOR}, surface, 0);
    REQUIRE(external_perimeter_count(generated) > 0);
    REQUIRE_FALSE(generated.fill_surfaces.empty());
    REQUIRE_FALSE(generated.fill_no_overlap_surfaces.empty());
    require_simple_generator_first_child_area_partition(generated, surface);
}

TEST_CASE("SimplePerimeterGenerator processes two layer islands independently", "[plugins][perimeter][simple-generator]")
{
    SECTION("single perimeter pass publishes output for both islands")
    {
        // The layer contains two disconnected islands in one region. The step
        // must run the selected perimeter generator once for each island, and
        // each island must receive fill areas clipped to its own geometry, not
        // to the union of the whole layer.
        const DynamicPrintConfig config = perimeter_config({{"perimeters", "1"}});
        ExPolygons islands;
        islands.push_back(rectangle_expolygon(-24., -5., -14., 5.));
        islands.push_back(rectangle_expolygon(14., -4., 24., 4.));

        const PerimeterMultiIslandRunCapture generated =
            run_perimeter_multi_island_case(config, {SIMPLE_PERIMETER_GENERATOR}, islands, 0);

        REQUIRE(generated.islands.size() == 2);
        for (size_t island_idx = 0; island_idx < generated.islands.size(); ++island_idx) {
            INFO("island " << island_idx);
            CHECK(external_perimeter_count(generated.islands[island_idx]) == 1);
            REQUIRE_FALSE(generated.islands[island_idx].fill_surfaces.empty());
            REQUIRE_FALSE(generated.islands[island_idx].fill_no_overlap_surfaces.empty());
            require_simple_generator_first_child_area_partition(generated.islands[island_idx], islands[island_idx]);
        }
    }

    SECTION("nested perimeter tree is independent for each island")
    {
        // With three requested shells, both disconnected islands should publish
        // their own root node and child chain. A bug in the pipeline would show
        // up here as only the first island being processed, or both islands
        // being flattened/merged into one tree.
        const DynamicPrintConfig config = perimeter_config({{"perimeters", "3"}});
        ExPolygons islands;
        islands.push_back(rectangle_expolygon(-25., -6., -13., 6.));
        islands.push_back(rectangle_expolygon(13., -6., 25., 6.));

        const PerimeterMultiIslandRunCapture generated =
            run_perimeter_multi_island_case(config, {SIMPLE_PERIMETER_GENERATOR}, islands, 0);

        REQUIRE(generated.islands.size() == 2);
        for (const PerimeterRunCapture &island_capture : generated.islands) {
            CHECK(external_perimeter_count(island_capture) == 3);
            require_published_tree_matches_perimeter_tree(island_capture, 3, 1);
            require_leaf_fill_area_consistency(island_capture);
        }
    }

    SECTION("plain island and holed island keep separate topology")
    {
        // The two islands deliberately have different topology: one plain
        // rectangle and one rectangle with a stable hole. The simple generator
        // should classify loops per island, so the plain island has only
        // contour loops while the holed island has contour and hole loops.
        const DynamicPrintConfig config = perimeter_config({{"perimeters", "2"}});
        ExPolygons islands;
        islands.push_back(rectangle_expolygon(-24., -8., -10., 8.));
        islands.push_back(rectangle_with_hole(-3., -3., 3., 3.));
        islands.back().translate(scale_i(17.), 0);

        const PerimeterMultiIslandRunCapture generated =
            run_perimeter_multi_island_case(config, {SIMPLE_PERIMETER_GENERATOR}, islands, 0);

        REQUIRE(generated.islands.size() == 2);

        const std::vector<SimpleLoopInfo> plain_loops = simple_loops(generated.islands[0]);
        REQUIRE(plain_loops.size() == 2);
        CHECK(loop_count_with_role(plain_loops, elrDefault) == 2);
        CHECK(loop_count_with_role(plain_loops, elrHole) == 0);
        require_published_tree_matches_perimeter_tree(generated.islands[0], 2, 1);

        const std::vector<SimpleLoopInfo> holed_loops = simple_loops(generated.islands[1]);
        REQUIRE(holed_loops.size() == 4);
        CHECK(loop_count_with_role(holed_loops, elrDefault) == 2);
        CHECK(loop_count_with_role(holed_loops, elrHole) == 2);
        require_published_tree_matches_perimeter_tree(generated.islands[1], 2, 2);
    }
}

TEST_CASE("SimplePerimeterGenerator writes loop and path properties", "[plugins][perimeter][simple-generator]")
{
    SECTION("contour-only island has external flow and shell indices on every loop")
    {
        // A plain rectangle produces one contour loop at each perimeter depth.
        // Each loop should be tagged as a perimeter loop, each child path should
        // carry external-perimeter flow, and shell_count should identify the
        // depth that generated it.
        const DynamicPrintConfig config = perimeter_config({{"perimeters", "3"}});
        const ExPolygon surface = rectangle_expolygon(-10., -10., 10., 10.);
        const PerimeterRunCapture generated = run_perimeter_case(config, {SIMPLE_PERIMETER_GENERATOR}, surface, 0);
        const std::vector<SimpleLoopInfo> loops = simple_loops(generated);

        REQUIRE(loops.size() == 3);
        CHECK(loop_count_with_role(loops, elrDefault) == 3);
        CHECK(loop_count_with_role(loops, elrHole) == 0);
        require_shell_indices(loops, 3, 1);
        require_loop_payloads_match_external_flow(generated, loops);
    }

    SECTION("separate central hole gets contour and hole loop properties")
    {
        // A rectangle with a centered hole that has enough spacing should emit
        // one contour loop and one hole loop at each perimeter depth. Both
        // classes still use the same external-perimeter path flow.
        const DynamicPrintConfig config = perimeter_config({{"perimeters", "3"}});
        const ExPolygon surface = rectangle_with_hole(-3., -3., 3., 3.);
        const PerimeterRunCapture generated = run_perimeter_case(config, {SIMPLE_PERIMETER_GENERATOR}, surface, 0);
        const std::vector<SimpleLoopInfo> loops = simple_loops(generated);

        REQUIRE(loops.size() == 6);
        CHECK(loop_count_with_role(loops, elrDefault) == 3);
        CHECK(loop_count_with_role(loops, elrHole) == 3);
        require_shell_indices(loops, 3, 2);
        require_loop_payloads_match_external_flow(generated, loops);
    }

    SECTION("near-edge hole stops being a hole when offsets fuse")
    {
        // When a hole is close to the outside wall, later offsets may no longer
        // contain a hole at all. The generated roles should reflect the actual
        // topology: at least the first loop is a hole, but not every requested
        // shell can remain a hole.
        const DynamicPrintConfig config = perimeter_config({{"perimeters", "3"}});
        const ExPolygon surface = close_to_edge_hole();
        const PerimeterRunCapture generated = run_perimeter_case(config, {SIMPLE_PERIMETER_GENERATOR}, surface, 0);
        const std::vector<SimpleLoopInfo> loops = simple_loops(generated);
        const size_t hole_count = loop_count_with_role(loops, elrHole);
        const size_t contour_count = loop_count_with_role(loops, elrDefault);

        CHECK(hole_count >= 1);
        CHECK(hole_count < 3);
        CHECK(contour_count >= 3);
        CHECK(loops.size() == hole_count + contour_count);
        CHECK(loop_count_with_shell(loops, 0) >= 2);
        CHECK(loop_count_with_shell(loops, 1) >= 1);
        require_loop_payloads_match_external_flow(generated, loops);
    }

    SECTION("zero requested perimeters creates no extrusion loops")
    {
        // perimeters=0 is handled by the generator itself. SeparateHoleContour
        // must not be responsible for removing the traversal seed used by the
        // perimeter step, so this test keeps perimeters_hole disabled.
        const DynamicPrintConfig config = perimeter_config({{"perimeters", "0"}});
        const ExPolygon surface = rectangle_with_hole(-3., -3., 3., 3.);
        const PerimeterRunCapture generated = run_perimeter_case(config, {SIMPLE_PERIMETER_GENERATOR}, surface, 0);

        CHECK(external_perimeter_count(generated) == 0);
        REQUIRE_FALSE(generated.fill_surfaces.empty());
        REQUIRE_FALSE(generated.fill_no_overlap_surfaces.empty());
        require_leaf_fill_area_consistency(generated);
    }
}

TEST_CASE("SimplePerimeterGenerator publishes the perimeter-node tree shape", "[plugins][perimeter][simple-generator]")
{
    SECTION("contour-only output keeps one child node per perimeter depth")
    {
        // The output structure should preserve the generator tree, not just the
        // flat list of loops: root node -> level 0 contour loop + child node ->
        // level 1 contour loop + child node -> level 2 contour loop.
        const DynamicPrintConfig config = perimeter_config({{"perimeters", "3"}});
        const ExPolygon surface = rectangle_expolygon(-10., -10., 10., 10.);
        const PerimeterRunCapture generated = run_perimeter_case(config, {SIMPLE_PERIMETER_GENERATOR}, surface, 0);

        require_published_tree_matches_perimeter_tree(generated, 3, 1);
    }

    SECTION("contour plus central hole keeps loop group before the child node")
    {
        // With a stable hole, each node level should contain the contour loop
        // and the hole loop generated at that depth, followed by the child node
        // that owns the next depth.
        const DynamicPrintConfig config = perimeter_config({{"perimeters", "2"}});
        const ExPolygon surface = rectangle_with_hole(-3., -3., 3., 3.);
        const PerimeterRunCapture generated = run_perimeter_case(config, {SIMPLE_PERIMETER_GENERATOR}, surface, 0);

        require_published_tree_matches_perimeter_tree(generated, 2, 2);
    }
}
