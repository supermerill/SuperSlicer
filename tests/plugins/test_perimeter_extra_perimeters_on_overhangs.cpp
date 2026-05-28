#include <catch2/catch.hpp>

#include "perimeter_test_helpers.hpp"

#include "libslic3r/AABBTreeLines.hpp"
#include "libslic3r/ClipperUtils.hpp"

namespace {
using namespace Slic3r;
using namespace Slic3r::Test::PerimeterPluginTests;

DynamicPrintConfig extra_overhang_config(std::initializer_list<std::pair<std::string, std::string>> overrides)
{
    DynamicPrintConfig config = perimeter_config({
        {"extra_perimeters_on_overhangs", "1"},
        {"perimeters", "2"},
        {"bridged_infill_margin", "200%"},
        {"bridge_precision", "10%"},
        {"infill_overlap", "0"}
    });
    for (const std::pair<std::string, std::string> &entry : overrides)
        config.set_deserialize_strict(entry.first, entry.second);
    return config;
}

ExPolygon overhang_target()
{
    return rectangle_expolygon(-12., -8., 12., 8.);
}

ExPolygon narrow_left_support()
{
    return rectangle_expolygon(-12., -8., -3., 8.);
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

double area_sum(const ExPolygons &areas)
{
    double out = 0.;
    for (const ExPolygon &area : areas)
        out += std::abs(area.area());
    return out;
}

double free_fill_area(const PerimeterRunCapture &capture)
{
    return area_sum(surface_expolygons(capture.fill_no_overlap_surfaces));
}

bool first_leaf_role_has(const ExtrusionEntity &entity, const ExtrusionRoleModifier role)
{
    if (entity.is_nop())
        return false;
    if (entity.is_leaf())
        return entity.role().has(role);
    for (size_t child_idx = 0; child_idx < entity.child_count(); ++child_idx)
        if (!entity.child(child_idx).empty())
            return first_leaf_role_has(entity.child(child_idx), role);
    return false;
}

size_t count_role_leaves(const ExtrusionEntity &entity, const ExtrusionRoleModifier role)
{
    if (entity.is_nop())
        return 0;
    if (entity.is_leaf())
        return entity.role().has(role) ? 1 : 0;

    size_t count = 0;
    for (size_t child_idx = 0; child_idx < entity.child_count(); ++child_idx)
        count += count_role_leaves(entity.child(child_idx), role);
    return count;
}

size_t count_role_loops(const ExtrusionEntity &entity, const ExtrusionRoleModifier role)
{
    if (entity.is_nop())
        return 0;

    size_t count = entity.is_loop() && entity.role().has(role) ? 1 : 0;
    if (!entity.is_leaf())
        for (size_t child_idx = 0; child_idx < entity.child_count(); ++child_idx)
            count += count_role_loops(entity.child(child_idx), role);
    return count;
}

bool role_leaves_disable_seams(const ExtrusionEntity &entity, const ExtrusionRoleModifier role)
{
    if (entity.is_nop())
        return true;
    if (entity.is_leaf() && entity.role().has(role)) {
        const ExtrusionAttributes *attributes = entity.get_property<ExtrusionAttributes>();
        return attributes != nullptr && attributes->no_seam;
    }

    for (size_t child_idx = 0; child_idx < entity.child_count(); ++child_idx)
        if (!role_leaves_disable_seams(entity.child(child_idx), role))
            return false;
    return true;
}

bool role_leaf_starts_closer_to_support_than_end(const ExtrusionEntity &entity,
                                                 const ExtrusionRoleModifier role,
                                                 const AABBTreeLines::LinesDistancer<Line> &support_distancer)
{
    if (entity.is_nop())
        return true;
    if (entity.is_leaf() && entity.has_polyline() && entity.role().has(role)) {
        const double first_distance = support_distancer.distance_from_lines<true>(entity.first_point());
        const double last_distance = support_distancer.distance_from_lines<true>(entity.last_point());
        return first_distance <= last_distance + SCALED_EPSILON;
    }

    for (size_t child_idx = 0; child_idx < entity.child_count(); ++child_idx)
        if (!role_leaf_starts_closer_to_support_than_end(entity.child(child_idx), role, support_distancer))
            return false;
    return true;
}

AABBTreeLines::LinesDistancer<Line> support_distancer_for(const ExPolygon &support)
{
    return AABBTreeLines::LinesDistancer<Line>{to_lines(to_polygons(ExPolygons{ support }))};
}

} // namespace

TEST_CASE("Extra perimeters on overhangs is inert when disabled or fully supported", "[plugins][perimeter][extra-overhang]")
{
    const ExPolygon target = overhang_target();
    const ExPolygon lower_support = narrow_left_support();
    const DynamicPrintConfig disabled = extra_overhang_config({{"extra_perimeters_on_overhangs", "0"}});
    const DynamicPrintConfig enabled = extra_overhang_config({});

    SECTION("Disabled setting leaves normal perimeters and fill domains unchanged")
    {
        // Geometry: layer 1 is a wide rectangle but the layer below supports
        // only its left side. With the option disabled, the post-process must
        // behave as a pure no-op even though an overhang exists.
        const PerimeterRunCapture baseline =
            run_perimeter_case(disabled, {SIMPLE_PERIMETER_GENERATOR}, target, 1);
        const PerimeterRunCapture post =
            run_perimeter_and_post_case_with_lower_area(
                disabled, {SIMPLE_PERIMETER_GENERATOR}, {EXTRA_PERIMETERS_ON_OVERHANGS},
                target, lower_support, 1);

        REQUIRE(external_perimeter_count(post) == external_perimeter_count(baseline));
        REQUIRE(free_fill_area(post) == Approx(free_fill_area(baseline)));
        require_leaf_fill_area_consistency(post);
    }

    SECTION("Fully supported island produces no extra overhang paths")
    {
        // Geometry: the target layer and the lower layer have the same island.
        // The detector may inspect the island, but there is no unsupported
        // infill area, so the result must stay identical to simple generation.
        const PerimeterRunCapture baseline =
            run_perimeter_case(enabled, {SIMPLE_PERIMETER_GENERATOR}, target, 1);
        const PerimeterRunCapture supported =
            run_perimeter_and_post_case_with_lower_area(
                enabled, {SIMPLE_PERIMETER_GENERATOR}, {EXTRA_PERIMETERS_ON_OVERHANGS},
                target, target, 1);

        REQUIRE(external_perimeter_count(supported) == external_perimeter_count(baseline));
        REQUIRE(count_role_leaves(supported.external_perimeters, ExtrusionRole::OverhangPerimeter) == 0);
        REQUIRE(free_fill_area(supported) == Approx(free_fill_area(baseline)));
        require_leaf_fill_area_consistency(supported);
    }
}

TEST_CASE("Extra perimeters on overhangs inserts anchors before normal perimeters", "[plugins][perimeter][extra-overhang]")
{
    const ExPolygon target = overhang_target();
    const ExPolygon lower_support = narrow_left_support();
    const DynamicPrintConfig enabled = extra_overhang_config({});

    const PerimeterRunCapture baseline =
        run_perimeter_case(enabled, {SIMPLE_PERIMETER_GENERATOR}, target, 1);
    const PerimeterRunCapture post =
        run_perimeter_and_post_case_with_lower_area(
            enabled, {SIMPLE_PERIMETER_GENERATOR}, {EXTRA_PERIMETERS_ON_OVERHANGS},
            target, lower_support, 1);

    // The post-process must add printable overhang paths, not only retag
    // existing loops. They are inserted into an unsortable wrapper before the
    // original perimeter tree so the G-code order can print the new anchors
    // before the standard island perimeters.
    REQUIRE(external_perimeter_count(post) > external_perimeter_count(baseline));
    REQUIRE(count_role_leaves(post.external_perimeters, ExtrusionRole::OverhangPerimeter) > 0);
    // Overhang anchors deliberately stay as paths. Turning them into loops
    // would let seam placement move their start point away from the supported
    // anchor.
    REQUIRE(count_role_loops(post.external_perimeters, ExtrusionRole::OverhangPerimeter) == 0);
    // Every anchor path is oriented from its most supported endpoint. Without
    // this, the first emitted overhang move may start in the air even though
    // the same path has a supported endpoint at the other end.
    const AABBTreeLines::LinesDistancer<Line> support_distancer = support_distancer_for(lower_support);
    REQUIRE(role_leaf_starts_closer_to_support_than_end(
        post.external_perimeters, ExtrusionRole::OverhangPerimeter, support_distancer));
    REQUIRE_FALSE(post.external_perimeters.can_sort());
    REQUIRE(post.external_perimeters.child_count() >= 2);
    REQUIRE(first_leaf_role_has(post.external_perimeters.child(0), ExtrusionRole::OverhangPerimeter));

    // Extra perimeters occupy part of the old infill domain. The strict fill
    // area published to the next steps must shrink accordingly while still
    // remaining a valid partition for later infill generation.
    REQUIRE(free_fill_area(post) < free_fill_area(baseline));
    require_leaf_fill_area_consistency(post);
}

TEST_CASE("Extra perimeters on overhangs disables seam placement on generated anchors", "[plugins][perimeter][extra-overhang]")
{
    const ExPolygon target = overhang_target();
    const ExPolygon lower_support = narrow_left_support();
    const DynamicPrintConfig enabled = extra_overhang_config({});

    const PerimeterRunCapture post =
        run_perimeter_and_post_case_with_lower_area(
            enabled, {SIMPLE_PERIMETER_GENERATOR}, {EXTRA_PERIMETERS_ON_OVERHANGS},
            target, lower_support, 1);

    // Generated overhang anchors are ordered from supported material outward.
    // Seam placement must not treat them as regular perimeter candidates,
    // because moving their start point can put the first extrusion segment over
    // unsupported air.
    REQUIRE(count_role_leaves(post.external_perimeters, ExtrusionRole::OverhangPerimeter) > 0);
    REQUIRE(role_leaves_disable_seams(post.external_perimeters, ExtrusionRole::OverhangPerimeter));
    require_leaf_fill_area_consistency(post);
}

TEST_CASE("Extra perimeters on overhangs honors region-local enablement", "[plugins][perimeter][extra-overhang]")
{
    const ExPolygon target = overhang_target();
    const ExPolygon lower_support = narrow_left_support();
    const ExPolygon right_region = rectangle_expolygon(0., -8., 12., 8.);

    DynamicPrintConfig config = extra_overhang_config({{"extra_perimeters_on_overhangs", "0"}});
    const PerimeterRunCapture disabled =
        run_perimeter_and_post_case_with_lower_area(
            config, {SIMPLE_PERIMETER_GENERATOR}, {EXTRA_PERIMETERS_ON_OVERHANGS},
            target, lower_support, 1);
    const PerimeterRunCapture local_enabled =
        run_perimeter_and_post_case_with_lower_area(
            config, {SIMPLE_PERIMETER_GENERATOR}, {EXTRA_PERIMETERS_ON_OVERHANGS},
            target, lower_support, 1,
            {{"extra_perimeters_on_overhangs", "1"}}, &right_region);

    // The default region disables the algorithm, while the right-side modifier
    // enables it exactly where the target island overhangs the lower layer.
    // This verifies that RegionSettings clipping selects only active areas and
    // preserves disabled areas for later infill.
    REQUIRE(count_role_leaves(disabled.external_perimeters, ExtrusionRole::OverhangPerimeter) == 0);
    REQUIRE(count_role_leaves(local_enabled.external_perimeters, ExtrusionRole::OverhangPerimeter) > 0);
    REQUIRE(free_fill_area(local_enabled) < free_fill_area(disabled));
    require_leaf_fill_area_consistency(local_enabled);
}
