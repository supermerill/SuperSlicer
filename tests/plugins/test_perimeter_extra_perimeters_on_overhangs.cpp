#include <catch2/catch.hpp>

#include "perimeter_test_helpers.hpp"
#include "plugin_test_helpers.hpp"

#include "libslic3r/AABBTreeLines.hpp"
#include "libslic3r/Api/host/Orchestrator.hpp"
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

ExPolygons free_fill_expolygons(const PerimeterRunCapture &capture)
{
    return union_ex(surface_expolygons(capture.fill_no_overlap_surfaces));
}

ExPolygons consumed_free_fill_expolygons(const PerimeterRunCapture &baseline,
                                         const PerimeterRunCapture &post)
{
    return diff_ex(free_fill_expolygons(baseline), free_fill_expolygons(post));
}

const ExtrusionEntity *first_overhang_wrapper(const ExtrusionEntity &root);

void require_wave_consumes_only_printed_volume(const DynamicPrintConfig &config,
                                               const ExPolygon &target,
                                               const ExPolygon &lower_support,
                                               const coord_t wave_spacing)
{
    const PerimeterRunCapture baseline =
        run_perimeter_case(config, {SIMPLE_PERIMETER_GENERATOR}, target, 1);
    const PerimeterRunCapture wave =
        run_perimeter_and_post_case_with_lower_area(
            config, {SIMPLE_PERIMETER_GENERATOR}, {EXTRA_PERIMETER_OVERHANG_WAVE},
            target, lower_support, 1);

    const ExPolygons wave_consumed = consumed_free_fill_expolygons(baseline, wave);
    REQUIRE(area_sum(wave_consumed) > 0.);

    const ExtrusionEntity *wrapper = first_overhang_wrapper(wave.external_perimeters);
    REQUIRE(wrapper != nullptr);
    REQUIRE(extrusion_length(*wrapper) > 0.);

    // The post-process removes a free-fill area, and every removed square
    // micron should correspond to a real generated centerline. Compare area
    // rather than mm3/mm: overhang extrusion may sag below the nominal layer,
    // but the centerline length times spacing is still the 2D budget that this
    // feature is allowed to consume from the next infill step. Corners and
    // short connectors overlap their own swept envelope, so a small excess in
    // the length-based estimate is acceptable; a large one would mean that the
    // plugin is printing more material than the free-fill area it returned.
    const double consumed_area = area_sum(wave_consumed);
    const double nominal_path_area = extrusion_length(*wrapper) * double(wave_spacing);
    const double tolerance = double(scale_i(0.04)) * double(scale_i(0.04));
    CHECK(nominal_path_area <= consumed_area * 1.05 + tolerance);
    CHECK(consumed_area <= nominal_path_area * 1.15 + tolerance);

    require_leaf_fill_area_consistency(wave);
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

bool role_leaves_allow_seams(const ExtrusionEntity &entity, const ExtrusionRoleModifier role)
{
    if (entity.is_nop())
        return true;
    if (entity.is_leaf() && entity.role().has(role)) {
        const ExtrusionAttributes *attributes = entity.get_property<ExtrusionAttributes>();
        return attributes != nullptr && !attributes->no_seam;
    }

    for (size_t child_idx = 0; child_idx < entity.child_count(); ++child_idx)
        if (!role_leaves_allow_seams(entity.child(child_idx), role))
            return false;
    return true;
}

bool role_leaves_are_not_reversible(const ExtrusionEntity &entity, const ExtrusionRoleModifier role)
{
    if (entity.is_nop())
        return true;
    if (entity.is_leaf() && entity.role().has(role))
        return !entity.can_reverse();

    for (size_t child_idx = 0; child_idx < entity.child_count(); ++child_idx)
        if (!role_leaves_are_not_reversible(entity.child(child_idx), role))
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

const ExtrusionEntity *first_overhang_wrapper(const ExtrusionEntity &root)
{
    if (root.is_nop() || root.is_leaf())
        return nullptr;
    for (size_t child_idx = 0; child_idx < root.child_count(); ++child_idx) {
        const ExtrusionEntity &child = root.child(child_idx);
        if (count_role_leaves(child, ExtrusionRole::OverhangPerimeter) > 0)
            return &child;
    }
    return nullptr;
}

size_t direct_overhang_zone_count(const ExtrusionEntity &wrapper)
{
    size_t count = 0;
    for (size_t child_idx = 0; child_idx < wrapper.child_count(); ++child_idx)
        if (count_role_leaves(wrapper.child(child_idx), ExtrusionRole::OverhangPerimeter) > 0)
            ++count;
    return count;
}

bool direct_overhang_zones_are_order_locked(const ExtrusionEntity &wrapper)
{
    for (size_t child_idx = 0; child_idx < wrapper.child_count(); ++child_idx) {
        const ExtrusionEntity &zone = wrapper.child(child_idx);
        if (count_role_leaves(zone, ExtrusionRole::OverhangPerimeter) == 0)
            continue;
        if (zone.can_sort() || zone.can_reverse())
            return false;
    }
    return true;
}

double point_distance_squared(const Point &lhs, const Point &rhs)
{
    return (lhs - rhs).cast<double>().squaredNorm();
}

bool zone_first_overhang_starts_after_supported_anchor(const ExtrusionEntity &zone,
                                                       const coord_t link_distance)
{
    const ExtrusionEntity *previous_leaf = nullptr;
    for (size_t child_idx = 0; child_idx < zone.child_count(); ++child_idx) {
        const ExtrusionEntity &leaf = zone.child(child_idx);
        if (!leaf.is_leaf() || !leaf.has_polyline())
            continue;

        if (leaf.role().has(ExtrusionRole::OverhangPerimeter)) {
            if (previous_leaf == nullptr ||
                previous_leaf->role().has(ExtrusionRole::OverhangPerimeter))
                return false;

            const double distance_squared =
                point_distance_squared(previous_leaf->last_point(), leaf.first_point());
            const double limit = double(link_distance);
            return distance_squared <= limit * limit;
        }

        previous_leaf = &leaf;
    }

    return false;
}

bool all_overhang_zones_start_from_supported_anchor(const ExtrusionEntity &wrapper,
                                                    const coord_t link_distance)
{
    bool saw_overhang_zone = false;
    for (size_t child_idx = 0; child_idx < wrapper.child_count(); ++child_idx) {
        const ExtrusionEntity &zone = wrapper.child(child_idx);
        if (count_role_leaves(zone, ExtrusionRole::OverhangPerimeter) == 0)
            continue;
        saw_overhang_zone = true;
        if (!zone_first_overhang_starts_after_supported_anchor(zone, link_distance))
            return false;
    }
    return saw_overhang_zone;
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

TEST_CASE("Extra perimeter overhang wave grows ordered zones from support", "[plugins][perimeter][extra-overhang][wave]")
{
    const ExPolygon target = overhang_target();
    const ExPolygon lower_support = narrow_left_support();
    const DynamicPrintConfig enabled = extra_overhang_config({});

    const PerimeterRunCapture baseline =
        run_perimeter_case(enabled, {SIMPLE_PERIMETER_GENERATOR}, target, 1);
    const PerimeterRunCapture post =
        run_perimeter_and_post_case_with_lower_area(
            enabled, {SIMPLE_PERIMETER_GENERATOR}, {EXTRA_PERIMETER_OVERHANG_WAVE},
            target, lower_support, 1);

    // Geometry: the lower layer supports only the left strip of a wider upper
    // island. The wave plugin starts at that supported strip, offsets outward,
    // and clips each wave to the unsupported fill domain. This should create
    // overhang anchors before the normal perimeter tree while preserving the
    // later fill/free-area partition.
    REQUIRE(external_perimeter_count(post) > external_perimeter_count(baseline));
    REQUIRE(count_role_leaves(post.external_perimeters, ExtrusionRole::OverhangPerimeter) > 0);
    REQUIRE(count_role_loops(post.external_perimeters, ExtrusionRole::OverhangPerimeter) == 0);
    // Wave anchors are open paths, not loops. Seam placement cannot rotate
    // them as closed loops, so they must not carry an unnecessary no-seam tag.
    REQUIRE(role_leaves_allow_seams(post.external_perimeters, ExtrusionRole::OverhangPerimeter));
    REQUIRE(role_leaves_are_not_reversible(post.external_perimeters, ExtrusionRole::OverhangPerimeter));

    const AABBTreeLines::LinesDistancer<Line> support_distancer = support_distancer_for(lower_support);
    REQUIRE(role_leaf_starts_closer_to_support_than_end(
        post.external_perimeters, ExtrusionRole::OverhangPerimeter, support_distancer));

    // The root is unsortable so the whole extra-overhang wrapper is emitted
    // before the original perimeter tree. Inside that wrapper, each individual
    // zone is unsortable/non-reversible because its paths must keep their
    // support-to-air order.
    const ExtrusionEntity *wrapper = first_overhang_wrapper(post.external_perimeters);
    REQUIRE(wrapper != nullptr);
    REQUIRE(wrapper->can_sort());
    REQUIRE(direct_overhang_zone_count(*wrapper) == 1);
    REQUIRE(direct_overhang_zones_are_order_locked(*wrapper));
    // The supported pre-line has a normal perimeter role, so it cannot be
    // merged into the first overhang path. It still belongs to the same locked
    // local group and must end next to the first overhang start, otherwise the
    // generated G-code travels back to the opposite side before printing the
    // first unsupported wave.
    REQUIRE(all_overhang_zones_start_from_supported_anchor(*wrapper, 2 * post.external_perimeter_width));

    REQUIRE(free_fill_area(post) < free_fill_area(baseline));
    require_leaf_fill_area_consistency(post);
}

TEST_CASE("Extra perimeter overhang wave keeps disconnected zones independently sortable", "[plugins][perimeter][extra-overhang][wave]")
{
    const ExPolygon target = rectangle_expolygon(-12., -8., 12., 8.);
    const ExPolygon central_support = rectangle_expolygon(-2., -8., 2., 8.);
    const DynamicPrintConfig enabled = extra_overhang_config({});

    const PerimeterRunCapture post =
        run_perimeter_and_post_case_with_lower_area(
            enabled, {SIMPLE_PERIMETER_GENERATOR}, {EXTRA_PERIMETER_OVERHANG_WAVE},
            target, central_support, 1);

    // Geometry: a narrow supported column below the middle of the island leaves
    // two independent unsupported regions, left and right. Each region must
    // keep its own strict wave order, but the two regions may be printed in
    // either order to reduce travel. The wrapper/zone hierarchy encodes that
    // exact contract for the downstream G-code ordering code.
    const ExtrusionEntity *wrapper = first_overhang_wrapper(post.external_perimeters);
    REQUIRE(wrapper != nullptr);
    REQUIRE(wrapper->can_sort());
    REQUIRE(direct_overhang_zone_count(*wrapper) == 2);
    REQUIRE(direct_overhang_zones_are_order_locked(*wrapper));
    REQUIRE(all_overhang_zones_start_from_supported_anchor(*wrapper, 2 * post.external_perimeter_width));
    REQUIRE(count_role_leaves(post.external_perimeters, ExtrusionRole::OverhangPerimeter) > 0);
    require_leaf_fill_area_consistency(post);
}

TEST_CASE("Extra perimeter overhang wave separates paths when wave jumps require travel", "[plugins][perimeter][extra-overhang][wave]")
{
    const ExPolygon target = rectangle_expolygon(-18., -8., 18., 8.);
    const ExPolygon lower_support = rectangle_expolygon(-18., -8., -10., 8.);
    const DynamicPrintConfig close_waves = extra_overhang_config({
        {"perimeters", "8"},
        {"overhangs_extrusion_spacing", "0.25"}
    });
    const DynamicPrintConfig far_waves = extra_overhang_config({
        {"perimeters", "8"},
        {"overhangs_extrusion_spacing", "1.2"}
    });

    const PerimeterRunCapture connected =
        run_perimeter_and_post_case_with_lower_area(
            close_waves, {SIMPLE_PERIMETER_GENERATOR}, {EXTRA_PERIMETER_OVERHANG_WAVE},
            target, lower_support, 1);
    const PerimeterRunCapture separated =
        run_perimeter_and_post_case_with_lower_area(
            far_waves, {SIMPLE_PERIMETER_GENERATOR}, {EXTRA_PERIMETER_OVERHANG_WAVE},
            target, lower_support, 1);

    // The wave plugin joins two successive wave fragments only when the
    // connector is shorter than two extrusion widths. A small spacing should
    // therefore build longer continuous paths, while a deliberately large
    // spacing must leave separate path leaves so the printer can travel.
    REQUIRE(count_role_leaves(connected.external_perimeters, ExtrusionRole::OverhangPerimeter) > 0);
    REQUIRE(count_role_leaves(separated.external_perimeters, ExtrusionRole::OverhangPerimeter) >
            count_role_leaves(connected.external_perimeters, ExtrusionRole::OverhangPerimeter));
    REQUIRE(role_leaves_allow_seams(separated.external_perimeters, ExtrusionRole::OverhangPerimeter));
    REQUIRE(role_leaves_are_not_reversible(separated.external_perimeters, ExtrusionRole::OverhangPerimeter));
    require_leaf_fill_area_consistency(connected);
    require_leaf_fill_area_consistency(separated);
}

TEST_CASE("Extra perimeter overhang wave consumes only the volume it prints", "[plugins][perimeter][extra-overhang][wave]")
{
    const DynamicPrintConfig enabled = extra_overhang_config({
        {"perimeters", "4"},
        {"overhangs_extrusion_spacing", "0.45"}
    });

    // The wave strategy may consume less free-fill area than the shrink-based
    // strategy, because it intentionally gives supported leftovers back to
    // later infill. It must not consume more area than its generated paths can
    // justify, otherwise those leftovers are printed twice.
    SECTION("one unsupported side")
    {
        require_wave_consumes_only_printed_volume(
            enabled, overhang_target(), narrow_left_support(), scale_i(0.45));
    }

    SECTION("two disconnected unsupported sides")
    {
        const ExPolygon target = rectangle_expolygon(-12., -8., 12., 8.);
        const ExPolygon central_support = rectangle_expolygon(-2., -8., 2., 8.);
        require_wave_consumes_only_printed_volume(
            enabled, target, central_support, scale_i(0.45));
    }
}

TEST_CASE("Extra perimeter overhang wave places the strategy selector beside the activation setting", "[plugins][perimeter][extra-overhang][wave]")
{
    Slic3r::Test::Plugins::ensure_plugin_test_runtime_initialized();

    const std::vector<Orchestrator::PluginUiFragment> fragments =
        Orchestrator::instance().ui_fragments_for_file("print.ui");
    size_t group_fragment_count = 0;
    const Orchestrator::PluginUiFragment *group_fragment = nullptr;
    for (const Orchestrator::PluginUiFragment &fragment : fragments) {
        if (fragment.fragment_id != EXTRA_PERIMETERS_ON_OVERHANGS)
            continue;
        ++group_fragment_count;
        group_fragment = &fragment;
    }

    // The wave implementation owns the explicit placement for this exclusive
    // group. The fallback Notes-page fragment must not be registered too,
    // otherwise the selector appears twice in the generated UI.
    REQUIRE(group_fragment_count == 1);
    REQUIRE(group_fragment != nullptr);
    CHECK(group_fragment->content.find("line:Extra perimeters") != std::string::npos);
    CHECK(group_fragment->content.find("insert$aftersetting$extra_perimeters_on_overhangs") != std::string::npos);
    CHECK(group_fragment->content.find(
        "exclusive_group_700_perimeter_post_process_extra_perimeters_on_overhangs_plugin") != std::string::npos);
}
