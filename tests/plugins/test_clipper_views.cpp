#include <catch2/catch.hpp>

#include "libslic3r/Api/host/Orchestrator.hpp"
#include "libslic3r/Api/plugin/cpp/ClipperViews.hpp"
#include "libslic3r/ExPolygon.hpp"
#include "libslic3r/Polygon.hpp"

namespace {
using namespace Slic3r;

Point point_mm(const double x, const double y)
{
    return Point(scale_i(x), scale_i(y));
}

Polygon rectangle_mm(const double min_x, const double min_y, const double max_x, const double max_y)
{
    Polygon polygon({
        point_mm(min_x, min_y),
        point_mm(max_x, min_y),
        point_mm(max_x, max_y),
        point_mm(min_x, max_y)
    });
    polygon.make_counter_clockwise();
    return polygon;
}

ExPolygon rectangle_with_two_holes()
{
    ExPolygon expolygon(rectangle_mm(0., 0., 20., 20.));
    expolygon.holes.push_back(rectangle_mm(2., 2., 5., 5.));
    expolygon.holes.push_back(rectangle_mm(8., 8., 11., 11.));
    for (Polygon &hole : expolygon.holes)
        hole.make_clockwise();
    return expolygon;
}

slic3r_api::Polygon view(const Polygon &polygon)
{
    return slic3r_api::Polygon(reinterpret_cast<const polygon_handle *>(&polygon));
}

slic3r_api::ExPolygon view(const ExPolygon &expolygon)
{
    return slic3r_api::ExPolygon(reinterpret_cast<const expolygon_handle *>(&expolygon));
}

void check_bbox(const c_bounding_box &bbox,
                const double min_x,
                const double min_y,
                const double max_x,
                const double max_y)
{
    CHECK(bbox.min.x == scale_i(min_x));
    CHECK(bbox.min.y == scale_i(min_y));
    CHECK(bbox.max.x == scale_i(max_x));
    CHECK(bbox.max.y == scale_i(max_y));
}

} // namespace

TEST_CASE("ClipperOperand path_count counts raw non-empty paths", "[plugins][clipper]")
{
    PluginStorage plugin_storage;
    storage_handle *storage = reinterpret_cast<storage_handle *>(&plugin_storage);
    slic3r_api::ClipperContext clipper(storage);

    slic3r_api::ClipperOperand empty = clipper.empty();
    CHECK(empty.path_count() == 0);
    CHECK(empty.empty());

    Polygon polygon = rectangle_mm(0., 0., 10., 10.);
    slic3r_api::ClipperOperand polygon_operand = clipper(view(polygon));
    CHECK(polygon_operand.path_count() == 1);
    CHECK(!polygon_operand.empty());

    ExPolygon expolygon = rectangle_with_two_holes();
    slic3r_api::ClipperOperand expolygon_operand = clipper(view(expolygon));
    CHECK(expolygon_operand.path_count() == 3);

    Polygon other_polygon = rectangle_mm(30., 0., 40., 10.);
    slic3r_api::ClipperOperand concatenated = clipper.empty();
    concatenated += polygon_operand;
    CHECK(concatenated.path_count() == 1);
    concatenated += clipper(view(other_polygon));
    CHECK(concatenated.path_count() == 2);

    slic3r_api::ClipperOperand unioned = slic3r_api::clipper_union(concatenated);
    CHECK(unioned.path_count() == 2);
    CHECK(!unioned.empty());
}

TEST_CASE("ClipperOperand bounding_box reads raw Clipper points", "[plugins][clipper]")
{
    PluginStorage plugin_storage;
    storage_handle *storage = reinterpret_cast<storage_handle *>(&plugin_storage);
    slic3r_api::ClipperContext clipper(storage);

    CHECK(clipper.empty().bounding_box().min.x == 0);
    CHECK(clipper.empty().bounding_box().min.y == 0);
    CHECK(clipper.empty().bounding_box().max.x == 0);
    CHECK(clipper.empty().bounding_box().max.y == 0);

    Polygon polygon = rectangle_mm(-2., 3., 10., 12.);
    check_bbox(clipper(view(polygon)).bounding_box(), -2., 3., 10., 12.);

    ExPolygon expolygon = rectangle_with_two_holes();
    check_bbox(clipper(view(expolygon)).bounding_box(), 0., 0., 20., 20.);

    Polygon other_polygon = rectangle_mm(30., -5., 40., 6.);
    slic3r_api::ClipperOperand concatenated = clipper(view(polygon));
    concatenated += clipper(view(other_polygon));
    check_bbox(concatenated.bounding_box(), -2., -5., 40., 12.);

    slic3r_api::ClipperOperand unioned = slic3r_api::clipper_union(concatenated);
    check_bbox(unioned.bounding_box(), -2., -5., 40., 12.);
}
