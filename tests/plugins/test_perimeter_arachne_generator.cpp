#include <catch2/catch.hpp>

#include "perimeter_test_helpers.hpp"

#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/Api/plugin/cpp/ExtrusionViews.hpp"
#include "libslic3r/ExtrusionEntity.hpp"
#include "libslic3r/ExtrusionProperty.hpp"

#include <algorithm>
#include <cmath>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace {
using namespace Slic3r;
using namespace Slic3r::Test::PerimeterPluginTests;

struct WidthCrossing
{
    float width = 0.f;
};

struct ArachneRegressionResult
{
    std::vector<PerimeterRunCapture> islands;
    std::set<int16_t> shell_indices;
    size_t loop_count = 0;
    double extrusion_length = 0.;
};

Polygon circle_polygon(const double center_x, const double center_y, const double radius, const size_t point_count)
{
    Points points;
    points.reserve(point_count);
    for (size_t idx = 0; idx < point_count; ++idx) {
        const double angle = 2. * PI * double(idx) / double(point_count);
        points.emplace_back(scale_i(center_x + radius * std::cos(angle)),
                            scale_i(center_y + radius * std::sin(angle)));
    }
    Polygon polygon(std::move(points));
    polygon.make_counter_clockwise();
    return polygon;
}

ExPolygon tapered_thin_surface()
{
    // A long closed island whose middle is always too thin for regular
    // multi-perimeter output. Its thickness still changes from left to right,
    // so Arachne should keep a single center extrusion but vary its width.
    return ExPolygon(Polygon({
        Point(scale_i(-14.), scale_i(-0.30)),
        Point(scale_i(-7.),  scale_i(-0.30)),
        Point(scale_i(7.),   scale_i(-0.42)),
        Point(scale_i(14.),  scale_i(-0.42)),
        Point(scale_i(14.),  scale_i(0.42)),
        Point(scale_i(7.),   scale_i(0.42)),
        Point(scale_i(-7.),  scale_i(0.30)),
        Point(scale_i(-14.), scale_i(0.30))
    }));
}

ExPolygon crescent_surface()
{
    // Difference between two overlapping disks. The result is a half-moon-like
    // island with a wide belly and narrow tips; with enough requested
    // perimeters, Arachne should use variable-width walls to cover the island
    // instead of leaving a large infill island behind.
    const ExPolygon outer(circle_polygon(0., 0., 12., 96));
    const ExPolygon cutter(circle_polygon(3.5, 0., 11., 96));
    ExPolygons crescent = diff_ex(outer, cutter);
    REQUIRE(crescent.size() == 1);
    return crescent.front();
}

bool segment_crosses_vertical_window(const Point &a,
                                     const Point &b,
                                     const coord_t x,
                                     const coord_t y_min,
                                     const coord_t y_max)
{
    if (a.x() == b.x())
        return a.x() == x && std::max(a.y(), b.y()) >= y_min && std::min(a.y(), b.y()) <= y_max;

    if ((a.x() < x && b.x() < x) || (a.x() > x && b.x() > x))
        return false;

    const double t = double(x - a.x()) / double(b.x() - a.x());
    if (t < 0. || t > 1.)
        return false;

    const double y = double(a.y()) + t * double(b.y() - a.y());
    return y >= double(y_min) && y <= double(y_max);
}

bool polyline_crosses_vertical_window(const ArcPolyline &polyline,
                                      const double x_mm,
                                      const double y_min_mm,
                                      const double y_max_mm)
{
    const Polyline simple = polyline.to_polyline();
    if (simple.points.size() < 2)
        return false;

    const coord_t x = scale_i(x_mm);
    const coord_t y_min = scale_i(y_min_mm);
    const coord_t y_max = scale_i(y_max_mm);
    for (size_t idx = 1; idx < simple.points.size(); ++idx)
        if (segment_crosses_vertical_window(simple.points[idx - 1], simple.points[idx], x, y_min, y_max))
            return true;
    return false;
}

void collect_width_crossings(const ExtrusionEntity &entity,
                             const double x_mm,
                             const double y_min_mm,
                             const double y_max_mm,
                             std::vector<WidthCrossing> &out)
{
    if (const ArcPolyline *polyline = entity.polyline_or_null()) {
        const ExtrusionAttributes *attributes = entity.get_property<ExtrusionAttributes>();
        if (attributes != nullptr && polyline_crosses_vertical_window(*polyline, x_mm, y_min_mm, y_max_mm))
            out.push_back({attributes->width});
        return;
    }

    if (!entity.is_leaf())
        for (const ExtrusionEntityUPtr &child : entity.children())
            if (child)
                collect_width_crossings(*child, x_mm, y_min_mm, y_max_mm, out);
}

size_t crossing_branch_count(const ExtrusionEntity &entity,
                             const double x_mm,
                             const double y_min_mm,
                             const double y_max_mm)
{
    if (entity.is_leaf())
        return entity.polyline_or_null() != nullptr &&
               polyline_crosses_vertical_window(entity.as_polyline(), x_mm, y_min_mm, y_max_mm) ? 1 : 0;

    if (!entity.is_collection())
        return polyline_crosses_vertical_window(entity.as_polyline(), x_mm, y_min_mm, y_max_mm) ? 1 : 0;

    size_t count = 0;
    for (const ExtrusionEntityUPtr &child : entity.children())
        if (child != nullptr)
            count += crossing_branch_count(*child, x_mm, y_min_mm, y_max_mm);
    return count;
}

float max_crossing_width(const std::vector<WidthCrossing> &crossings)
{
    REQUIRE_FALSE(crossings.empty());
    float out = 0.f;
    for (const WidthCrossing &crossing : crossings)
        out = std::max(out, crossing.width);
    return out;
}

double covered_area_ratio(const ExPolygon &surface, const ExtrusionEntity &extrusions)
{
    Polygons covered_polygons;
    extrusions.polygons_covered_by_width(covered_polygons, float(scale_d(0.02)));
    ExPolygons covered = union_ex(covered_polygons);
    ExPolygons missed = diff_ex(surface, covered);
    return area(missed) / surface.area();
}

std::string config_length(const coord_t value)
{
    return std::to_string(unscaled(value));
}

DynamicPrintConfig arachne_regression_config(
    const coord_t external_spacing,
    const coord_t internal_spacing,
    const size_t perimeter_count,
    const double layer_height,
    std::initializer_list<std::pair<std::string, std::string>> overrides = {})
{
    DynamicPrintConfig config = perimeter_config({});
    config.set_deserialize_strict("layer_height", std::to_string(layer_height));
    config.set_deserialize_strict("first_layer_height", std::to_string(layer_height));
    config.set_deserialize_strict("perimeters", std::to_string(perimeter_count));
    config.set_deserialize_strict("external_perimeter_extrusion_width", config_length(external_spacing));
    config.set_deserialize_strict("external_perimeter_extrusion_spacing", config_length(external_spacing));
    config.set_deserialize_strict("perimeter_extrusion_width", config_length(internal_spacing));
    config.set_deserialize_strict("perimeter_extrusion_spacing", config_length(internal_spacing));
    for (const std::pair<std::string, std::string> &override_value : overrides)
        config.set_deserialize_strict(override_value.first, override_value.second);
    return config;
}

void inspect_arachne_extrusion(const ExtrusionEntity &entity, ArachneRegressionResult &result)
{
    if (entity.is_nop())
        return;

    if (const slic3r_api::EPropertyPerimeter *perimeter =
            entity.get_property<slic3r_api::EPropertyPerimeter>())
        result.shell_indices.insert(perimeter->shell_count());

    if (entity.is_loop()) {
        Points loop_points;
        entity.collect_points(loop_points);
        REQUIRE(loop_points.size() >= 2);
        REQUIRE(loop_points.front() == loop_points.back());
        ++result.loop_count;
    }

    if (entity.is_leaf()) {
        REQUIRE(entity.has_polyline());
        Points points;
        entity.collect_points(points);
        REQUIRE(points.size() >= 2);
        REQUIRE(entity.length() > 0.);
        REQUIRE(entity.get_property<ExtrusionAttributes>() != nullptr);
        return;
    }

    for (const ExtrusionEntityUPtr &child : entity.children()) {
        REQUIRE(child != nullptr);
        inspect_arachne_extrusion(*child, result);
    }
}

ArachneRegressionResult run_arachne_regression(
    const Polygons &polygons,
    const coord_t external_spacing,
    const coord_t internal_spacing,
    const size_t perimeter_count,
    const double layer_height,
    std::initializer_list<std::pair<std::string, std::string>> overrides = {})
{
    ExPolygons islands = union_ex(polygons);
    REQUIRE_FALSE(islands.empty());

    const DynamicPrintConfig config = arachne_regression_config(
        external_spacing, internal_spacing, perimeter_count, layer_height, overrides);
    PerimeterMultiIslandRunCapture generated = run_perimeter_multi_island_case(
        config, {ARACHNE_PERIMETER_GENERATOR}, islands, 0);

    ArachneRegressionResult result;
    result.islands = std::move(generated.islands);
    REQUIRE_FALSE(result.islands.empty());
    for (size_t island_idx = 0; island_idx < result.islands.size(); ++island_idx) {
        INFO("Arachne regression island " << island_idx);
        const PerimeterRunCapture &capture = result.islands[island_idx];
        REQUIRE(external_perimeter_count(capture) > 0);
        require_leaf_fill_area_consistency(capture);
        result.extrusion_length += extrusion_length(capture.external_perimeters);
        inspect_arachne_extrusion(capture.external_perimeters, result);
    }
    return result;
}

bool has_published_fill_area(const ArachneRegressionResult &result)
{
    for (const PerimeterRunCapture &capture : result.islands)
        if (!capture.fill_surfaces.empty() && !capture.fill_no_overlap_surfaces.empty())
            return true;
    return false;
}
}

TEST_CASE("ArachnePerimeterGenerator publishes variable-width perimeter output", "[plugins][perimeter][arachne]") {
    // The Arachne STEP_PERIMETER plugin consume island payload and publish both variable-width perimeter extrusions
    // and fill surfaces for the generated inner contour.
    const DynamicPrintConfig config = perimeter_config({{"perimeters", "2"}});
    const ExPolygon surface = rectangle_with_hole_expolygon();

    const PerimeterRunCapture generated =
        run_perimeter_case(config, {ARACHNE_PERIMETER_GENERATOR}, surface, 0);
    REQUIRE(external_perimeter_count(generated) > 0);
    REQUIRE(extrusion_length(generated.external_perimeters) > 0.);
    REQUIRE_FALSE(generated.fill_surfaces.empty());
    REQUIRE_FALSE(generated.fill_no_overlap_surfaces.empty());
    require_leaf_fill_area_consistency(generated);
}

TEST_CASE("ArachnePerimeterGenerator emits one wider line through a variable thin area", "[plugins][perimeter][arachne]")
{
    // The island is a tapered thin ribbon. One perimeter generation pass cannot
    // fit several regular side-by-side walls in the narrow cross-sections, so
    // Arachne should produce exactly one extrusion crossing each thin section,
    // and the crossing in the wider section should carry a larger width.
    const DynamicPrintConfig config = perimeter_config({{"perimeters", "1"}});
    const PerimeterRunCapture generated =
        run_perimeter_case(config, {ARACHNE_PERIMETER_GENERATOR}, tapered_thin_surface(), 0);

    std::vector<WidthCrossing> narrow_crossings;
    std::vector<WidthCrossing> wider_crossings;
    collect_width_crossings(generated.external_perimeters, -8., -1., 1., narrow_crossings);
    collect_width_crossings(generated.external_perimeters, 0., -1., 1., wider_crossings);

    REQUIRE(crossing_branch_count(generated.external_perimeters, -8., -1., 1.) == 1);
    REQUIRE(crossing_branch_count(generated.external_perimeters, 0., -1., 1.) == 1);
    const float narrow_width = max_crossing_width(narrow_crossings);
    const float wider_width = max_crossing_width(wider_crossings);
    CHECK(narrow_width < wider_width);
    CHECK(wider_width - narrow_width > 0.02f);
}

TEST_CASE("ArachnePerimeterGenerator covers a crescent surface with enough perimeters", "[plugins][perimeter][arachne]")
{
    // A half-moon has thin tips and a broader center. With enough requested
    // perimeters, the variable-width perimeter set should cover nearly the
    // whole island by width, leaving only tiny numerical clipping residue.
    const ExPolygon surface = crescent_surface();
    const DynamicPrintConfig config = perimeter_config({{"perimeters", "9"}});
    const PerimeterRunCapture generated =
        run_perimeter_case(config, {ARACHNE_PERIMETER_GENERATOR}, surface, 0);

    REQUIRE(external_perimeter_count(generated) > 0);
    CHECK(covered_area_ratio(surface, generated.external_perimeters) < 0.02);
}

TEST_CASE("Arachne - Closed ExtrusionLine",
          "[plugins][perimeter][arachne][regression][ArachneClosedExtrusionLine]")
{
    const Polygon polygon = {
        Point(-40000000, 10000000), Point(-62480000, 10000000), Point(-62480000, -7410000),
        Point(-58430000, -7330000), Point(-58400000, -5420000), Point(-58720000, -4710000),
        Point(-58940000, -3870000), Point(-59020000, -3000000)
    };

    const ArachneRegressionResult generated =
        run_arachne_regression({polygon}, 407079, 407079, 5, 0.2);
    REQUIRE(generated.loop_count > 0);
}

// Distilled from GitHub issue #8472, where wall_distribution_count == 3 could
// omit the middle perimeter.
TEST_CASE("Arachne - Missing perimeter - #8472",
          "[plugins][perimeter][arachne][regression][ArachneMissingPerimeter8472]")
{
    const Polygon polygon = {
        Point(-9000000, 8054793), Point(7000000, 8054793), Point(7000000, 10211874),
        Point(-8700000, 10211874), Point(-9000000, 9824444)
    };

    const ArachneRegressionResult generated = run_arachne_regression(
        {polygon}, 437079, 437079, 3, 0.2, {{"wall_distribution_count", "3"}});
    REQUIRE(generated.shell_indices.size() == 3);
}

// Distilled from GitHub issue #8593. Specific rotations of a symmetrical gear
// tooth used to lose part of an extrusion.
TEST_CASE("Arachne - #8593 - Missing a part of the extrusion",
          "[plugins][perimeter][arachne][regression][ArachneMissingPartOfExtrusion8593]")
{
    const Polygon original = {
        Point(1800000, 28500000), Point(1100000, 30000000), Point(1000000, 30900000),
        Point(600000, 32300000), Point(-600000, 32300000), Point(-1000000, 30900000),
        Point(-1100000, 30000000), Point(-1800000, 29000000)
    };

    for (const double angle : {0., -PI / 2., -PI / 15.}) {
        INFO("Input rotation " << angle);
        Polygon polygon = original;
        if (angle != 0.)
            polygon.rotate(angle);
        run_arachne_regression(
            {polygon}, 377079, 377079, 3, 0.2,
            {{"min_bead_width", "0.315"}, {"wall_transition_angle", "40"},
             {"wall_transition_length", "1"}});
    }
}

// Distilled from GitHub issue #8573.
TEST_CASE("Arachne - #8573 - A gap in the perimeter - 1",
          "[plugins][perimeter][arachne][regression][ArachneGapInPerimeter8573_1]")
{
    const Polygon polygon = {
        Point(13960000, 500000), Point(13920000, 1210000), Point(13490000, 2270000),
        Point(12960000, 3400000), Point(12470000, 4320000), Point(12160000, 4630000),
        Point(12460000, 3780000), Point(12700000, 2850000), Point(12880000, 1910000),
        Point(12950000, 1270000), Point(13000000, 500000)
    };

    run_arachne_regression({polygon}, 407079, 407079, 2, 0.2);
}

// Distilled from GitHub issue #8444.
TEST_CASE("Arachne - #8444 - A gap in the perimeter - 2",
          "[plugins][perimeter][arachne][regression][ArachneGapInPerimeter8444_2]")
{
    const Polygon polygon = {
        Point(14413938, 3825902), Point(16817613, 711749), Point(19653030, 67154),
        Point(20075592, 925370), Point(20245428, 1339788), Point(20493219, 2121894),
        Point(20570295, 2486625), Point(20616559, 2835232), Point(20631964, 3166882),
        Point(20591800, 3858877), Point(19928267, 2153012), Point(19723020, 1829802),
        Point(19482017, 1612364), Point(19344810, 1542433), Point(19200249, 1500902),
        Point(19047680, 1487200), Point(18631073, 1520777), Point(18377524, 1567627),
        Point(18132517, 1641174), Point(17896307, 1741360), Point(17669042, 1868075),
        Point(17449999, 2021790)
    };

    run_arachne_regression({polygon}, 594159, 594159, 2, 0.4);
}

// Distilled from GitHub issue #8528. A hole could appear where the perimeter
// count changes from six to seven.
TEST_CASE("Arachne - #8528 - A hole when number of perimeters is changing",
          "[plugins][perimeter][arachne][regression][ArachneHoleOnPerimetersChange8528]")
{
    const Polygon polygon = {
        Point(-30000000, 27650000), Point(-30000000, 33500000), Point(-40000000, 33500000),
        Point(-40500000, 33500000), Point(-41100000, 33400000), Point(-41600000, 33200000),
        Point(-42100000, 32900000), Point(-42600000, 32600000), Point(-43000000, 32200000),
        Point(-43300000, 31700000), Point(-43600000, 31200000), Point(-43800000, 30700000),
        Point(-43900000, 30100000), Point(-43900000, 29600000), Point(-43957080, 25000000),
        Point(-39042920, 25000000), Point(-39042920, 27650000)
    };

    run_arachne_regression(
        {polygon}, 814159, 814159, 5, 0.4, {{"min_bead_width", "0.66"}});
}

// Distilled from GitHub issue #8555. These contours represent successive
// layers whose single-perimeter behavior used to vary unexpectedly.
TEST_CASE("Arachne - #8555 - Inconsistent single perimeter",
          "[plugins][perimeter][arachne][regression][ArachneInconsistentSinglePerimeter8555]")
{
    const Polygons layer_polygons = {
        Polygon({Point(5527411, -38490007), Point(11118814, -36631169), Point(13529600, -36167120),
                 Point(11300145, -36114514), Point(10484024, -36113916), Point(5037323, -37985945),
                 Point(4097054, -39978866)}),
        Polygon({Point(5566841, -38517205), Point(11185208, -36649404), Point(13462719, -36211009),
                 Point(11357290, -36161329), Point(10583855, -36160763), Point(5105952, -38043516),
                 Point(4222019, -39917031)}),
        Polygon({Point(5606269, -38544404), Point(11251599, -36667638), Point(13391666, -36255700),
                 Point(10683552, -36207653), Point(5174580, -38101085), Point(4346981, -39855197)}),
        Polygon({Point(5645699, -38571603), Point(11317993, -36685873), Point(13324786, -36299588),
                 Point(10783383, -36254499), Point(5243209, -38158655), Point(4471947, -39793362)}),
        Polygon({Point(5685128, -38598801), Point(11384385, -36704108), Point(13257907, -36343476),
                 Point(10883211, -36301345), Point(5311836, -38216224), Point(4596909, -39731528)}),
        Polygon({Point(5724558, -38626000), Point(11450778, -36722343), Point(13191026, -36387365),
                 Point(10983042, -36348191), Point(5380466, -38273795), Point(4721874, -39669693)})
    };

    for (size_t layer_idx = 0; layer_idx < layer_polygons.size(); ++layer_idx) {
        INFO("Regression layer " << layer_idx);
        run_arachne_regression({layer_polygons[layer_idx]}, 417809, 417809, 2, 0.15);
    }
}

// Distilled from GitHub issue #8633. Open perimeter endpoints used to become
// shorter than the equivalent closed perimeter.
TEST_CASE("Arachne - #8633 - Shorter open perimeter",
          "[plugins][perimeter][arachne][regression][ArachneShorterOpenPerimeter8633]")
{
    const Polygons variants = {
        Polygon({Point(6507498, 4189461), Point(6460382, 3601960), Point(6390896, 3181097),
                 Point(6294072, 2765838), Point(6170293, 2357794), Point(7090581, 2045388),
                 Point(7232821, 2514293), Point(7344089, 2991501), Point(7423910, 3474969),
                 Point(7471937, 3962592), Point(7487443, 4436235), Point(6515575, 4436235)}),
        Polygon({Point(6507498, 4189461), Point(6460382, 3601960), Point(6390896, 3181097),
                 Point(6294072, 2765838), Point(6170293, 2357794), Point(6917958, 1586830),
                 Point(7090552, 2045398), Point(7232821, 2514293), Point(7344089, 2991501),
                 Point(7423910, 3474969), Point(7471937, 3962592), Point(7487443, 4436235),
                 Point(6515575, 4436235)})
    };

    for (size_t variant_idx = 0; variant_idx < variants.size(); ++variant_idx) {
        INFO("Regression variant " << variant_idx);
        run_arachne_regression(
            {variants[variant_idx]}, 617809, 617809, 1, 0.15,
            {{"min_bead_width", "0.51"}, {"min_feature_size", "0.15"},
             {"wall_transition_length", "0.6"}});
    }
}

// Distilled from GitHub issue #8597, which triggered invalid iterator
// decrementing while removing small areas.
TEST_CASE("Arachne - #8597 - removeSmallAreas",
          "[plugins][perimeter][arachne][regression][ArachneRemoveSmallAreas8597]")
{
    const Polygons polygons = {
        Polygon({Point(-38768167, -3636556), Point(-38763631, -3617883), Point(-38763925, -3617820),
                 Point(-38990169, -3919539), Point(-38928506, -3919539)}),
        Polygon({Point(-39521732, -4480560), Point(-39383333, -4398498), Point(-39119825, -3925307),
                 Point(-39165608, -3926212), Point(-39302205, -3959445), Point(-39578719, -4537002)})
    };

    const ArachneRegressionResult generated =
        run_arachne_regression(polygons, 407079, 407079, 2, 0.2);
    REQUIRE(generated.shell_indices.size() == 1);
}

// This fixture exposed an open PolylineStitcher result which removed the
// remaining infill domain.
TEST_CASE("Arachne - Missing infill",
          "[plugins][perimeter][arachne][regression][ArachneMissingInfill]")
{
    const Polygons polygons = {
        Polygon({Point(5525881, 3649657), Point(452351, -2035297), Point(-1014702, -2144286),
                 Point(-5142096, -9101108), Point(5525882, -9101108)}),
        Polygon({Point(1415524, -2217520), Point(1854189, -2113857), Point(1566974, -2408538)}),
        Polygon({Point(-42854, -3771357), Point(310500, -3783332), Point(77735, -4059215)})
    };

    const ArachneRegressionResult generated =
        run_arachne_regression(polygons, 357079, 357079, 2, 0.2);
    REQUIRE(has_published_fill_area(generated));
}

// Distilled from GitHub issue #8849. The historical length assertion was
// disabled, so this migration deliberately checks only the common output
// invariants rather than inventing a new threshold.
TEST_CASE("Arachne - #8849 - Missing part of model",
          "[plugins][perimeter][arachne][regression][ArachneMissingPart8849]")
{
    const Polygon polygon = {
        Point(-29700000, -10600000), Point(-28200000, -10600000), Point(20000000, -10600000),
        Point(20000000, -9900000), Point(-28200000, -9900000), Point(-28200000, 0),
        Point(-29700000, 0)
    };

    run_arachne_regression({polygon}, 449999, 757079, 2, 0.32);
}

// Distilled from GitHub issue #8446. Intersecting linear Voronoi edges used
// to create perimeters outside the intended domain.
TEST_CASE("Arachne - #8446 - Degenerated Voronoi diagram - Linear edges",
          "[plugins][perimeter][arachne][regression][ArachneDegeneratedDiagram8446LinearEdges]")
{
    const Polygon polygon = {
        Point(42240656, 9020315), Point(4474248, 42960681), Point(-4474248, 42960681),
        Point(-4474248, 23193537), Point(-6677407, 22661038), Point(-8830542, 21906307),
        Point(-9702935, 21539826), Point(-13110431, 19607811), Point(-18105334, 15167780),
        Point(-20675743, 11422461), Point(-39475413, 17530840), Point(-42240653, 9020315)
    };

    const ArachneRegressionResult generated =
        run_arachne_regression({polygon}, 407079, 407079, 1, 0.2);
    REQUIRE(generated.extrusion_length <= scale_i(211.5));
}

// Distilled from GitHub issue #8846. A degenerate parabolic Voronoi edge used
// to intersect a linear edge and create excessive perimeter length.
TEST_CASE("Arachne - #8846 - Degenerated Voronoi diagram - One Parabola",
          "[plugins][perimeter][arachne][regression][ArachneDegeneratedDiagram8846OneParabola]")
{
    const Polygon outer = {
        Point(101978540, -41304489), Point(101978540, 41304489), Point(94709788, 42514051),
        Point(94709788, 48052315), Point(93352716, 48052315), Point(93352716, 42514052),
        Point(75903540, 42514051), Point(75903540, 48052315), Point(74546460, 48052315),
        Point(74546460, 42514052), Point(69634788, 42514051), Point(69634788, 48052315),
        Point(68277708, 48052315), Point(68277708, 42514051), Point(63366040, 42514051),
        Point(63366040, 48052315), Point(62008960, 48052315), Point(62008960, 42514051),
        Point(57097292, 42514051), Point(57097292, 48052315), Point(55740212, 48052315),
        Point(55740212, 42514052), Point(50828540, 42514052), Point(50828540, 48052315),
        Point(49471460, 48052315), Point(49471460, 42514051), Point(25753540, 42514051),
        Point(25753540, 48052315), Point(24396460, 48052315), Point(24396460, 42514051),
        Point(19484790, 42514052), Point(19484790, 48052315), Point(18127710, 48052315),
        Point(18127710, 42514051), Point(-5590210, 42514051), Point(-5590210, 48052315),
        Point(-6947290, 48052315), Point(-6947290, 42514051), Point(-11858960, 42514051),
        Point(-11858960, 48052315), Point(-13216040, 48052315), Point(-13216040, 42514051),
        Point(-18127710, 42514051), Point(-18127710, 48052315), Point(-19484790, 48052315),
        Point(-19484790, 42514052), Point(-49471460, 42514051), Point(-49471460, 48052315),
        Point(-50828540, 48052315), Point(-50828540, 42514052), Point(-55740212, 42514052),
        Point(-55740212, 48052315), Point(-57097292, 48052315), Point(-57097292, 42514051),
        Point(-68277708, 42514051), Point(-68277708, 48052315), Point(-69634788, 48052315),
        Point(-69634788, 42514051), Point(-74546460, 42514052), Point(-74546460, 48052315),
        Point(-75903540, 48052315), Point(-75903540, 42514051), Point(-80815204, 42514051),
        Point(-80815204, 48052315), Point(-82172292, 48052315), Point(-82172292, 42514051),
        Point(-87083956, 42514051), Point(-87083956, 48052315), Point(-88441044, 48052315),
        Point(-88441044, 42514051), Point(-99621460, 42514051), Point(-99621460, 48052315),
        Point(-100978540, 48052315), Point(-100978540, 42528248), Point(-101978540, 41304489),
        Point(-101978540, -41304489), Point(-100978540, -48052315), Point(-99621460, -48052315)
    };
    const Polygon inner = {
        Point(-100671460, -40092775), Point(-100671460, 40092775),
        Point(100671460, 40092775), Point(100671460, -40092775)
    };

    const ArachneRegressionResult generated =
        run_arachne_regression({outer, inner}, 607079, 607079, 1, 0.2);
    REQUIRE(generated.extrusion_length <= scale_i(1335.));
}

// Distilled from GitHub issue #9357. Two intersecting parabolic Voronoi edges
// used to create excessive perimeter length.
TEST_CASE("Arachne - #9357 - Degenerated Voronoi diagram - Two parabolas",
          "[plugins][perimeter][arachne][regression][ArachneDegeneratedDiagram9357TwoParabolas]")
{
    const Polygon polygon = {
        Point(78998946, -11733905), Point(40069507, -7401251), Point(39983905, -6751055),
        Point(39983905, 8251054), Point(79750000, 10522762), Point(79983905, 10756667),
        Point(79983905, 12248946), Point(79950248, 12504617), Point(79709032, 12928156),
        Point(79491729, 13102031), Point(78998946, 13233905), Point(38501054, 13233905),
        Point(37258117, 12901005), Point(36349000, 11991885), Point(36100868, 11392844),
        Point(36016095, 10748947), Point(36016095, -6751054), Point(35930493, -7401249),
        Point(4685798, -11733905)
    };

    const ArachneRegressionResult generated =
        run_arachne_regression({polygon}, 407079, 407079, 1, 0.2);
    REQUIRE(generated.extrusion_length <= scale_i(256.));
}

// Distilled from GitHub issue #8846. Voronoi edges intersecting the input
// segments used to create perimeters outside the intended domain.
TEST_CASE("Arachne - #8846 - Degenerated Voronoi diagram - Voronoi edges intersecting input segment",
          "[plugins][perimeter][arachne][regression][ArachneDegeneratedDiagram8846IntersectingInputSegment]")
{
    const Polygon polygon = {
        Point(60000000, 58000000), Point(-20000000, 53229451), Point(49312250, 53229452),
        Point(49443687, 53666225), Point(55358348, 50908580), Point(53666223, 49443687),
        Point(53229452, 49312250), Point(53229452, -49312250), Point(53666014, -49443623),
        Point(-10000000, -58000000), Point(60000000, -58000000)
    };

    const ArachneRegressionResult generated =
        run_arachne_regression({polygon}, 407079, 407079, 1, 0.32);
    REQUIRE(generated.extrusion_length <= scale_i(500.));
}

// Distilled from GitHub issue #10034. Rotating the input by PI / 6 did not
// repair this non-planar Voronoi diagram.
TEST_CASE("Arachne - #10034 - Degenerated Voronoi diagram - That wasn't fixed by rotation by PI / 6",
          "[plugins][perimeter][arachne][regression][ArachneDegeneratedDiagram10034RotationNotWorks]")
{
    const Polygon polygon_0 = {
        Point(43612632, -25179766), Point(58456010, 529710), Point(51074898, 17305660),
        Point(49390982, 21042355), Point(48102357, 23840161), Point(46769686, 26629546),
        Point(45835761, 28472742), Point(45205450, 29623133), Point(45107431, 29878059),
        Point(45069846, 30174950), Point(45069846, 50759533), Point(-45069846, 50759533),
        Point(-45069852, 29630557), Point(-45105780, 29339980), Point(-45179725, 29130704),
        Point(-46443313, 26398986), Point(-52272109, 13471493), Point(-58205450, 95724),
        Point(-29075091, -50359531), Point(29075086, -50359531)
    };
    const Polygon polygon_1 = {
        Point(-37733905, 45070445), Point(-37813254, 45116257), Point(-39353851, 47784650),
        Point(-39353851, 47876274), Point(-38632470, 49125743), Point(-38553121, 49171555),
        Point(-33833475, 49171555), Point(-33754126, 49125743), Point(-33032747, 47876277),
        Point(-33032747, 47784653), Point(-34007855, 46095721), Point(-34573350, 45116257),
        Point(-34652699, 45070445)
    };
    const Polygon polygon_2 = {
        Point(-44016799, 40706401), Point(-44116953, 40806555), Point(-44116953, 46126289),
        Point(-44016799, 46226443), Point(-42211438, 46226443), Point(-42132089, 46180631),
        Point(-40591492, 43512233), Point(-40591492, 43420609), Point(-41800123, 41327194),
        Point(-42132089, 40752213), Point(-42211438, 40706401)
    };
    const Polygon polygon_3 = {
        Point(6218189, 10966609), Point(6138840, 11012421), Point(4598238, 13680817),
        Point(4598238, 13772441), Point(6138840, 16440843), Point(6218189, 16486655),
        Point(9299389, 16486655), Point(9378738, 16440843), Point(10919340, 13772441),
        Point(10919340, 13680817), Point(10149039, 12346618), Point(9378738, 11012421),
        Point(9299389, 10966609)
    };
    const Polygon polygon_4 = {
        Point(13576879, 6718065), Point(13497530, 6763877), Point(11956926, 9432278),
        Point(11956926, 9523902), Point(13497528, 12192302), Point(13576877, 12238114),
        Point(16658079, 12238112), Point(16737428, 12192300), Point(18278031, 9523904),
        Point(18278031, 9432280), Point(17507729, 8098077), Point(16737428, 6763877),
        Point(16658079, 6718065)
    };

    run_arachne_regression(
        {polygon_0, polygon_1, polygon_2, polygon_3, polygon_4}, 407079, 407079, 1, 0.2);
}

TEST_CASE("Arachne - SPE-1837 - No perimeters generated",
          "[plugins][perimeter][arachne][regression][ArachneNoPerimetersGeneratedSPE1837]")
{
    const Polygon polygon = {
        Point(10000000, 10000000), Point(-10000000, 10000000),
        Point(-10000000, -10000000), Point(10000000, -10000000)
    };

    const ArachneRegressionResult generated =
        run_arachne_regression({polygon}, 300000, 700000, 1, 0.2);
    REQUIRE_FALSE(generated.islands.empty());
}
