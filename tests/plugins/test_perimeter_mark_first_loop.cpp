#include <catch2/catch.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "perimeter_test_helpers.hpp"

#include "libslic3r/Api/host/Orchestrator.hpp"
#include "libslic3r/Api/host/Plugin.hpp"
#include "libslic3r/Api/plugin/c/steps/slic3r_step_perimeter.h"
#include "libslic3r/ExtrusionEntity.hpp"
#include "libslic3r/ExtrusionRole.hpp"

namespace {
using namespace Slic3r;
using namespace Slic3r::Test::PerimeterPluginTests;

struct LoopStats
{
    size_t loop_count = 0;
    size_t first_loop_count = 0;
    std::vector<int16_t> first_loop_shells;
};

struct DirectNode
{
    explicit DirectNode(uint32_t perimeter_idx = 0)
        : extrusions(true)
    {
        node.extrusions = reinterpret_cast<extrusion_entity_handle *>(&extrusions);
        node.perimeter_idx = perimeter_idx;
        node.perimeter_needed = perimeter_idx + 1;
    }

    perimeter_node node = {};
    ExtrusionEntity extrusions;
    std::vector<std::unique_ptr<DirectNode>> children;
    std::vector<perimeter_node *> child_handles;

    DirectNode &add_child(uint32_t perimeter_idx)
    {
        children.emplace_back(new DirectNode(perimeter_idx));
        children.back()->node.parent = &node;
        refresh();
        return *children.back();
    }

    void refresh()
    {
        child_handles.clear();
        child_handles.reserve(children.size());
        for (std::unique_ptr<DirectNode> &child : children) {
            child->node.parent = &node;
            child->refresh();
            child_handles.push_back(&child->node);
        }
        node.children = child_handles.empty() ? nullptr : child_handles.data();
        node.child_count = uint32_t(child_handles.size());
        node.extrusions = reinterpret_cast<extrusion_entity_handle *>(&extrusions);
    }
};

void collect_loop_stats(const ExtrusionEntity &entity, LoopStats &out)
{
    if (const ExtrusionPropertyLoopRole *loop_role = entity.get_property<ExtrusionPropertyLoopRole>()) {
        const ExtrusionLoopRole flags = loop_role->perimeter_role();
        if ((flags & elrDefault) != 0 || (flags & elrHole) != 0 || (flags & elrInternal) != 0) {
            ++out.loop_count;
            if ((flags & elrFirstLoop) != 0) {
                ++out.first_loop_count;
                out.first_loop_shells.push_back(loop_role->perimeter_idx);
            }
        }
    }

    for (size_t child_idx = 0; child_idx < entity.child_count(); ++child_idx)
        collect_loop_stats(entity.child(child_idx), out);
}

LoopStats loop_stats(const ExtrusionEntity &entity)
{
    LoopStats out;
    collect_loop_stats(entity, out);
    return out;
}

ExtrusionPath closed_test_path(ExtrusionRole role, double inset)
{
    ExtrusionPath path(ExtrusionAttributes(role, ExtrusionFlow(0.1, 0.4f, 0.2f)), nullptr, true);
    path.polyline().append(Point(scale_i(-8. + inset), scale_i(-8. + inset)));
    path.polyline().append(Point(scale_i(8. - inset), scale_i(-8. + inset)));
    path.polyline().append(Point(scale_i(8. - inset), scale_i(8. - inset)));
    path.polyline().append(Point(scale_i(-8. + inset), scale_i(8. - inset)));
    path.polyline().append(Point(scale_i(-8. + inset), scale_i(-8. + inset)));
    return path;
}

ExtrusionEntity loop_entity(ExtrusionLoopRole role, int16_t shell_idx, ExtrusionRole extrusion_role, double inset)
{
    ExtrusionEntity loop(ExtrusionEntity::Children(), false, true, true);
    ExtrusionPropertyLoopRole &loop_role = loop.get_or_add_property<ExtrusionPropertyLoopRole>();
    loop_role.perimeter_idx = shell_idx;
    loop_role.set_perimeter_role(role);
    loop.append_child(closed_test_path(extrusion_role, inset));
    return loop;
}

perimeter_generation_module_instance mark_first_loop_module(Print &print)
{
    Orchestrator &orchestrator = Orchestrator::instance();
    Plugin *plugin = orchestrator.get_plugin(MARK_FIRST_LOOP);
    REQUIRE(plugin != nullptr);

    plugin_host_context host_context =
        orchestrator.prepare_plugin_host_context(PERIMETER_GENERATION_MODULE, plugin, &print);
    plugin_run_context run_context =
        orchestrator.prepare_plugin_run_context(PERIMETER_GENERATION_MODULE, plugin, &host_context);
    run_ctx_perimeter_generation_module payload = {};
    run_context.data = &payload;
    plugin->setup(run_context, 1);
    plugin->setup_run(run_context);
    plugin->run(run_context);
    REQUIRE(payload.module.vt != nullptr);
    return payload.module;
}

void run_mark_first_loop_module(perimeter_node &root, Print &print)
{
    perimeter_generation_module_instance module = mark_first_loop_module(print);
    perimeter_generation_context context = {};
    context.root = &root;
    REQUIRE(module.vt->end != nullptr);
    module.vt->end(module.ctx, nullptr, &context);
}

}

TEST_CASE("Mark first loop tags only the innermost simple perimeter", "[plugins][perimeter][first-loop]")
{
    // A plain rectangle with three requested perimeters produces a simple chain
    // of nested perimeter nodes. Only the deepest generated loop should receive
    // FIRST_LOOP; outer loops still have normal loop metadata but are not the
    // first loop touched by infill.
    const DynamicPrintConfig config = perimeter_config({{"perimeters", "3"}});
    const PerimeterRunCapture run =
        run_perimeter_case(config, {SIMPLE_PERIMETER_GENERATOR, MARK_FIRST_LOOP}, rectangle_expolygon(-10., -10., 10., 10.), 0);

    const LoopStats stats = loop_stats(external_perimeters(run));
    REQUIRE(stats.loop_count == 3);
    REQUIRE(stats.first_loop_count == 1);
    REQUIRE(stats.first_loop_shells.size() == 1);
    CHECK(stats.first_loop_shells.front() == 2);
}

TEST_CASE("Mark first loop tags terminal contour and hole loops", "[plugins][perimeter][first-loop]")
{
    // Contours and holes share the same depth chain but both are real perimeter
    // loops at the terminal shell. The module should mark the innermost contour
    // and the innermost hole, while leaving the external shell unmarked.
    const DynamicPrintConfig config = perimeter_config({{"perimeters", "2"}, {"perimeters_hole", "2"}});
    const PerimeterRunCapture run =
        run_perimeter_case(config, {SIMPLE_PERIMETER_GENERATOR, MARK_FIRST_LOOP}, rectangle_with_hole_expolygon(), 0);

    const LoopStats stats = loop_stats(external_perimeters(run));
    REQUIRE(stats.loop_count == 4);
    REQUIRE(stats.first_loop_count == 2);
    for (const int16_t shell_idx : stats.first_loop_shells)
        CHECK(shell_idx == 1);
}

TEST_CASE("Mark first loop falls back to the nearest generated parent", "[plugins][perimeter][first-loop]")
{
    // This direct tree reproduces the edge case where a generated node has a
    // child area, but that child never produced perimeter extrusion. The empty
    // child must not block the parent loop from being considered innermost.
    PreparedPerimeterPrint prepared;
    prepare_cube_print(prepared, perimeter_config({}));

    DirectNode root(0);
    root.extrusions.append_child(loop_entity(elrDefault, 0, ExtrusionRole::ExternalPerimeter, 0.));
    root.add_child(1);
    root.refresh();

    run_mark_first_loop_module(root.node, prepared.print);

    const LoopStats stats = loop_stats(root.extrusions);
    REQUIRE(stats.loop_count == 1);
    REQUIRE(stats.first_loop_count == 1);
    CHECK(stats.first_loop_shells.front() == 0);
}

TEST_CASE("Mark first loop ignores gap fill and thin wall loops", "[plugins][perimeter][first-loop]")
{
    // Gap fill and thin walls may live in the perimeter generation tree, but
    // they are not shell loops. Even if they carry loop-like metadata in a
    // malformed or transitional tree, this module must leave them untagged.
    PreparedPerimeterPrint prepared;
    prepare_cube_print(prepared, perimeter_config({}));

    DirectNode root(0);
    root.extrusions.append_child(loop_entity(elrDefault, 0, ExtrusionRole::ExternalPerimeter, 0.));

    DirectNode &child = root.add_child(1);
    child.extrusions.append_child(loop_entity(elrDefault, 1, ExtrusionRole::GapFill, 1.));
    child.extrusions.append_child(loop_entity(elrDefault, 1, ExtrusionRole::ThinWall, 2.));
    root.refresh();

    run_mark_first_loop_module(root.node, prepared.print);

    const LoopStats root_stats = loop_stats(root.extrusions);
    const LoopStats child_stats = loop_stats(child.extrusions);
    REQUIRE(root_stats.loop_count == 1);
    REQUIRE(child_stats.loop_count == 2);
    CHECK(root_stats.first_loop_count == 1);
    CHECK(child_stats.first_loop_count == 0);
}
