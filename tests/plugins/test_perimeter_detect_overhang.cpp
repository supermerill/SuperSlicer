#include <catch2/catch.hpp>

#include "perimeter_test_helpers.hpp"

#include "libslic3r/ExtrusionProperty.hpp"

#include <algorithm>
#include <limits>

namespace {
using namespace Slic3r;
using namespace Slic3r::Test::PerimeterPluginTests;

struct DetectedOverhangStats
{
    size_t perimeter_leaves = 0;
    size_t overhang_properties = 0;
    size_t full_flow = 0;
    size_t full_speed = 0;
    size_t dynamic_flow = 0;
    size_t dynamic_speed = 0;
    size_t bridge_role = 0;
    size_t dynamic_without_bridge = 0;
    double max_distance = 0.;
};

DynamicPrintConfig detect_overhang_config(std::initializer_list<std::pair<std::string, std::string>> overrides)
{
    DynamicPrintConfig config = perimeter_config({
        {"overhangs", "1"},
        {"overhangs_flow_ratio", "100"},
        {"overhangs_type", "nozzle"},
        {"perimeters", "1"}
    });
    for (const std::pair<std::string, std::string> &entry : overrides)
        config.set_deserialize_strict(entry.first, entry.second);
    return config;
}

ExPolygon detected_overhang_target()
{
    return rectangle_expolygon(-10., -5., 10., 5.);
}

ExPolygon half_width_lower_support()
{
    return rectangle_expolygon(-10., -5., 5., 5.);
}

ExPolygon near_full_lower_support()
{
    return rectangle_expolygon(-10., -5., 9.5, 5.);
}

ExPolygon triangle_lower_support()
{
    return ExPolygon(Polygon({
        Point(scale_i(-10.), scale_i(-5.)),
        Point(scale_i(11.), scale_i(0.)),
        Point(scale_i(-10.), scale_i(5.))
    }));
}

void collect_detected_overhang_stats(const ExtrusionEntity &entity, DetectedOverhangStats &out)
{
    if (entity.is_nop())
        return;

    if (entity.is_leaf()) {
        const ExtrusionAttributes *attributes = entity.get_property<ExtrusionAttributes>();
        if (attributes == nullptr || !attributes->extrusion_role().is_perimeter())
            return;

        ++out.perimeter_leaves;
        if (attributes->extrusion_role().is_bridge())
            ++out.bridge_role;

        const ExtrusionPropertyOverhang *overhang = entity.get_property<ExtrusionPropertyOverhang>();
        if (overhang == nullptr)
            return;

        ++out.overhang_properties;
        if (overhang->has_full_overhangs_flow)
            ++out.full_flow;
        if (overhang->has_full_overhangs_speed)
            ++out.full_speed;
        if (overhang->has_dynamic_overhangs_flow)
            ++out.dynamic_flow;
        if (overhang->has_dynamic_overhangs_speed)
            ++out.dynamic_speed;
        if ((overhang->has_dynamic_overhangs_flow || overhang->has_dynamic_overhangs_speed) &&
            !attributes->extrusion_role().is_bridge())
            ++out.dynamic_without_bridge;

        out.max_distance = std::max(out.max_distance, double(overhang->end_distance_from_prev_layer));
        return;
    }

    for (size_t child_idx = 0; child_idx < entity.child_count(); ++child_idx)
        collect_detected_overhang_stats(entity.child(child_idx), out);
}

DetectedOverhangStats detected_overhang_stats(const PerimeterRunCapture &capture)
{
    DetectedOverhangStats out;
    collect_detected_overhang_stats(capture.external_perimeters, out);
    return out;
}

PerimeterRunCapture run_detect_overhang_case(const DynamicPrintConfig &config,
                                             const ExPolygon &lower_support)
{
    return run_perimeter_and_post_case_with_lower_area(
        config, {SIMPLE_PERIMETER_GENERATOR}, {DETECT_OVERHANG},
        detected_overhang_target(), lower_support, 1);
}

} // namespace

TEST_CASE("Detect overhang marks near unsupported spans as dynamic only", "[plugins][perimeter][detect-overhang]")
{
    // The current layer is a rectangle, and the layer below is only slightly
    // shorter on the right. That creates a real unsupported perimeter span, but
    // the configured thresholds are deliberately much larger than the gap. The
    // plugin should keep the span as a normal perimeter role and attach dynamic
    // overhang metadata for later speed/flow interpolation.
    const DynamicPrintConfig config = detect_overhang_config({
        {"overhangs_width", "5"},
        {"overhangs_width_speed", "5"}
    });
    const PerimeterRunCapture capture = run_detect_overhang_case(config, near_full_lower_support());
    const DetectedOverhangStats stats = detected_overhang_stats(capture);

    REQUIRE(stats.perimeter_leaves > 0);
    REQUIRE(stats.overhang_properties > 0);
    CHECK(stats.full_flow == 0);
    CHECK(stats.full_speed == 0);
    CHECK(stats.dynamic_flow > 0);
    CHECK(stats.dynamic_speed > 0);
    CHECK(stats.bridge_role == 0);
    CHECK(stats.dynamic_without_bridge == stats.overhang_properties);
}

TEST_CASE("Detect overhang marks large unsupported spans as full flow and speed", "[plugins][perimeter][detect-overhang]")
{
    // The lower layer supports only the left half of the rectangle. The right
    // perimeter is far beyond both thresholds, so it should become a full
    // overhang for flow and speed. Full overhang paths are tagged as bridge
    // perimeter so downstream code can use bridge-style handling.
    const DynamicPrintConfig config = detect_overhang_config({
        {"overhangs_width", "0.2"},
        {"overhangs_width_speed", "0.2"}
    });
    const PerimeterRunCapture capture = run_detect_overhang_case(config, half_width_lower_support());
    const DetectedOverhangStats stats = detected_overhang_stats(capture);

    REQUIRE(stats.overhang_properties > 0);
    CHECK(stats.full_flow > 0);
    CHECK(stats.full_speed > 0);
    CHECK(stats.full_flow > stats.dynamic_flow);
    CHECK(stats.full_speed > stats.dynamic_speed);
    CHECK(stats.bridge_role >= std::max(stats.full_flow, stats.full_speed));
    CHECK(stats.max_distance > 0.2);
}

TEST_CASE("Detect overhang separates flow and speed thresholds", "[plugins][perimeter][detect-overhang]")
{
    // Flow and speed have independent thresholds. With a half-supported
    // rectangle, the unsupported distance is large enough to cross one
    // threshold while remaining below the other when we invert the two values.
    SECTION("flow threshold is crossed before speed threshold") {
        const DynamicPrintConfig config = detect_overhang_config({
            {"overhangs_width", "0.2"},
            {"overhangs_width_speed", "5"}
        });
        const PerimeterRunCapture capture = run_detect_overhang_case(config, half_width_lower_support());
        const DetectedOverhangStats stats = detected_overhang_stats(capture);

        REQUIRE(stats.overhang_properties > 0);
        CHECK(stats.full_flow > 0);
        CHECK(stats.full_speed == 0);
        CHECK(stats.dynamic_speed > 0);
        CHECK(stats.bridge_role > 0);
    }

    SECTION("speed threshold is crossed before flow threshold") {
        const DynamicPrintConfig config = detect_overhang_config({
            {"overhangs_width", "5"},
            {"overhangs_width_speed", "0.2"}
        });
        const PerimeterRunCapture capture = run_detect_overhang_case(config, half_width_lower_support());
        const DetectedOverhangStats stats = detected_overhang_stats(capture);

        REQUIRE(stats.overhang_properties > 0);
        CHECK(stats.full_flow == 0);
        CHECK(stats.full_speed > 0);
        CHECK(stats.dynamic_flow > 0);
        CHECK(stats.bridge_role > 0);
    }
}

TEST_CASE("Detect overhang samples varying support distance along one contour", "[plugins][perimeter][detect-overhang]")
{
    // A triangular lower island creates a support boundary that crosses the
    // rectangular current layer. Different points on the same right-side
    // perimeter are therefore at different distances from support. This checks
    // that the plugin splits and annotates more than one overhang fragment
    // instead of applying one coarse value to the whole contour.
    const DynamicPrintConfig config = detect_overhang_config({
        {"overhangs_width", "5"},
        {"overhangs_width_speed", "5"}
    });
    const PerimeterRunCapture capture = run_detect_overhang_case(config, triangle_lower_support());
    const DetectedOverhangStats stats = detected_overhang_stats(capture);

    REQUIRE(stats.overhang_properties > 1);
    CHECK(stats.dynamic_flow > 1);
    CHECK(stats.dynamic_speed > 1);
    CHECK(stats.full_flow == 0);
    CHECK(stats.full_speed == 0);
    CHECK(stats.max_distance > 0.);
}

TEST_CASE("Detect overhang preserves disabled overhang regions", "[plugins][perimeter][detect-overhang]")
{
    // The unsupported side is covered by a region where overhang detection is
    // disabled. RegionSettings still clips the perimeter by region, so the
    // important contract is that disabled clips are copied back unchanged
    // rather than silently dropping that part of the path.
    const DynamicPrintConfig config = detect_overhang_config({
        {"overhangs_width", "0.2"},
        {"overhangs_width_speed", "0.2"}
    });
    const ExPolygon disabled_right_half = rectangle_expolygon(0., -5., 10., 5.);
    const PerimeterRunCapture capture = run_perimeter_and_post_case_with_lower_area(
        config, {SIMPLE_PERIMETER_GENERATOR}, {DETECT_OVERHANG},
        detected_overhang_target(), half_width_lower_support(), 1,
        {{"overhangs", "0"}}, &disabled_right_half);
    const DetectedOverhangStats stats = detected_overhang_stats(capture);

    REQUIRE(stats.perimeter_leaves > 0);
    CHECK(stats.overhang_properties == 0);
    CHECK(stats.bridge_role == 0);
    CHECK(extrusion_length(capture.external_perimeters) > 0.);
}
