///|/ Copyright (c) SuperSlicer 2026 Durand Remi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/

/*
Tests for the public extrusion area splitter.

The fixtures deliberately provide complete, non-overlapping partitions. The
API contract leaves overlap and missing-coverage behavior undefined, so these
tests focus on stable handles, source order, preserved extrusion state and the
exact ArcPolyline geometry reconstructed after temporary clipping.
*/

#include <catch2/catch.hpp>

#include <cmath>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

#include "libslic3r/Api/plugin/c/slic3r_extrusion_entity.h"
#include "libslic3r/Api/plugin/cpp/ExtrusionViews.hpp"
#include "libslic3r/ExPolygon.hpp"
#include "libslic3r/ExtrusionEntity.hpp"
#include "libslic3r/Geometry/ArcWelder.hpp"

namespace {

using namespace Slic3r;

struct CallbackCapture
{
    std::vector<extrusion_entity_handle *> handles;
    std::vector<uint32_t> area_indices;
};

ExPolygon rectangle(coord_t min_x, coord_t min_y, coord_t max_x, coord_t max_y);
ExtrusionPath straight_path(const Points &points);
extrusion_entity_handle *entity_handle(ExtrusionEntity &entity);
const expolygon_collection_handle *area_handle(const ExPolygons &areas);
std::vector<const expolygon_collection_handle *> area_handles(const std::vector<ExPolygons> &areas);
void collect_fragment(extrusion_entity_handle *fragment, uint32_t area_index, void *user_data);

ExPolygon rectangle(coord_t min_x, coord_t min_y, coord_t max_x, coord_t max_y)
{
    return ExPolygon({ Point(min_x, min_y), Point(max_x, min_y),
                       Point(max_x, max_y), Point(min_x, max_y) });
}

ExtrusionPath straight_path(const Points &points)
{
    const ExtrusionAttributes attributes(
        ExtrusionRole::Perimeter,
        ExtrusionFlow(0.08, 0.4f, 0.2f));
    return ExtrusionPath(ArcPolyline(points), attributes, nullptr, true);
}

extrusion_entity_handle *entity_handle(ExtrusionEntity &entity)
{
    return reinterpret_cast<extrusion_entity_handle *>(&entity);
}

const expolygon_collection_handle *area_handle(const ExPolygons &areas)
{
    return reinterpret_cast<const expolygon_collection_handle *>(&areas);
}

std::vector<const expolygon_collection_handle *> area_handles(const std::vector<ExPolygons> &areas)
{
    std::vector<const expolygon_collection_handle *> out;
    out.reserve(areas.size());
    for (const ExPolygons &area : areas)
        out.push_back(area_handle(area));
    return out;
}

void collect_fragment(extrusion_entity_handle *fragment, uint32_t area_index, void *user_data)
{
    CallbackCapture &capture = *static_cast<CallbackCapture *>(user_data);
    capture.handles.push_back(fragment);
    capture.area_indices.push_back(area_index);
}

} // namespace

TEST_CASE("Extrusion leaves split by area in source order",
          "[plugins][extrusion][split-by-areas]")
{
    const coord_t unit = scale_i(1.);
    ExtrusionPath leaf = straight_path({ Point(0, 0), Point(30 * unit, 0) });
    leaf.add_property(ExtrusionPropertySpeed(42.f, 900.f));

    const std::vector<ExPolygons> areas = {
        { rectangle(-unit, -unit, 10 * unit, unit) },
        { rectangle(10 * unit, -unit, 20 * unit, unit) },
        { rectangle(20 * unit, -unit, 31 * unit, unit) }
    };
    const std::vector<const expolygon_collection_handle *> handles = area_handles(areas);
    CallbackCapture capture;
    capture.handles.reserve(3);
    capture.area_indices.reserve(3);

    const raw_extrusion_split_status status = extrusion_split_leaf_by_areas(
        entity_handle(leaf), handles.data(), uint32_t(handles.size()), SCALED_EPSILON,
        collect_fragment, &capture);

    REQUIRE(status == RAW_EXTRUSION_SPLIT_STATUS_SUCCESS);
    REQUIRE(leaf.child_count() == 3);
    CAPTURE(leaf.can_sort(), leaf.is_continuous(),
            leaf.child(0).last_point().x(), leaf.child(0).last_point().y(),
            leaf.child(1).first_point().x(), leaf.child(1).first_point().y(),
            leaf.child(1).last_point().x(), leaf.child(1).last_point().y(),
            leaf.child(2).first_point().x(), leaf.child(2).first_point().y());
    CHECK_FALSE(leaf.can_sort());
    CHECK(leaf.is_continuous());
    CHECK(capture.area_indices == std::vector<uint32_t>{ 0, 1, 2 });

    for (size_t child_idx = 0; child_idx < leaf.child_count(); ++child_idx) {
        const ExtrusionEntity &child = leaf.child(child_idx);
        REQUIRE(capture.handles[child_idx] == entity_handle(const_cast<ExtrusionEntity &>(child)));
        CHECK(child.can_reverse());
        REQUIRE(child.get_property<ExtrusionPropertySpeed>() != nullptr);
        CHECK(child.get_property<ExtrusionPropertySpeed>()->speed_mm_per_s == 42.f);
        CHECK(child.get_property<ExtrusionPropertySpeed>()->accel_mm_per_s2 == 900.f);
    }

    CHECK(leaf.child(0).first_point() == Point(0, 0));
    CHECK(leaf.child(0).last_point() == leaf.child(1).first_point());
    CHECK(leaf.child(1).last_point() == leaf.child(2).first_point());
    CHECK(leaf.child(2).last_point() == Point(30 * unit, 0));
}

TEST_CASE("A single extrusion area preserves the leaf identity",
          "[plugins][extrusion][split-by-areas]")
{
    const coord_t unit = scale_i(1.);
    ExtrusionPath leaf = straight_path({ Point(0, 0), Point(10 * unit, 0) });
    const ExPolygons area = { rectangle(-unit, -unit, 11 * unit, unit) };
    const expolygon_collection_handle *handle = area_handle(area);
    extrusion_entity_handle *const original_handle = entity_handle(leaf);
    CallbackCapture capture;
    capture.handles.reserve(1);
    capture.area_indices.reserve(1);

    REQUIRE(extrusion_split_leaf_by_areas(
                original_handle, &handle, 1, 0, collect_fragment, &capture) ==
            RAW_EXTRUSION_SPLIT_STATUS_SUCCESS);

    CHECK(leaf.has_polyline());
    CHECK(leaf.child_count() == 0);
    REQUIRE(capture.handles.size() == 1);
    CHECK(capture.handles.front() == original_handle);
    CHECK(capture.area_indices == std::vector<uint32_t>{ 0 });

    // The callback is optional and does not change the successful no-op path.
    REQUIRE(extrusion_split_leaf_by_areas(
                original_handle, &handle, 1, SCALED_EPSILON, nullptr, nullptr) ==
            RAW_EXTRUSION_SPLIT_STATUS_SUCCESS);
    CHECK(leaf.has_polyline());

    const std::vector<ExPolygons> areas_with_empty = { {}, area };
    const std::vector<const expolygon_collection_handle *> handles = area_handles(areas_with_empty);
    CallbackCapture empty_capture;
    REQUIRE(extrusion_split_leaf_by_areas(
                original_handle, handles.data(), uint32_t(handles.size()), SCALED_EPSILON,
                collect_fragment, &empty_capture) == RAW_EXTRUSION_SPLIT_STATUS_SUCCESS);
    CHECK(empty_capture.handles == std::vector<extrusion_entity_handle *>{ original_handle });
    CHECK(empty_capture.area_indices == std::vector<uint32_t>{ 1 });
    CHECK(leaf.has_polyline());
}

TEST_CASE("Adjacent pieces owned by one area become one extrusion fragment",
          "[plugins][extrusion][split-by-areas]")
{
    const coord_t unit = scale_i(1.);
    ExtrusionPath leaf = straight_path({ Point(0, 0), Point(20 * unit, 0) });

    // Area zero intentionally contains two touching polygons. Clipper may
    // return them as separate intervals, but they have identical ownership and
    // are adjacent along the source path.
    const std::vector<ExPolygons> areas = {
        { rectangle(-unit, -unit, 5 * unit, unit),
          rectangle(5 * unit, -unit, 10 * unit, unit) },
        { rectangle(10 * unit, -unit, 21 * unit, unit) }
    };
    const std::vector<const expolygon_collection_handle *> handles = area_handles(areas);
    CallbackCapture capture;
    capture.handles.reserve(2);
    capture.area_indices.reserve(2);

    REQUIRE(extrusion_split_leaf_by_areas(
                entity_handle(leaf), handles.data(), uint32_t(handles.size()), SCALED_EPSILON,
                collect_fragment, &capture) == RAW_EXTRUSION_SPLIT_STATUS_SUCCESS);

    REQUIRE(leaf.child_count() == 2);
    CHECK(capture.area_indices == std::vector<uint32_t>{ 0, 1 });
    CHECK(leaf.child(0).first_point() == Point(0, 0));
    CHECK(leaf.child(0).last_point() == Point(10 * unit, 0));
}

TEST_CASE("Arc and Z geometry survive extrusion area splitting",
          "[plugins][extrusion][split-by-areas]")
{
    const coord_t unit = scale_i(1.);
    ArcPolyline source;
    source.append(Point(unit, 0));
    source.append(Geometry::ArcWelder::Segment(
        Point(0, unit), float(unit), Geometry::ArcWelder::Orientation::CCW));
    source.set_z_offset(0, 100);
    source.set_z_offset(1, 900);
    const ExtrusionAttributes attributes(
        ExtrusionRole::Perimeter,
        ExtrusionFlow(0.08, 0.4f, 0.2f));
    ExtrusionPath leaf(std::move(source), attributes, nullptr, true);

    const coord_t split_x = scale_i(0.7);
    const std::vector<ExPolygons> areas = {
        { rectangle(split_x, -unit, 2 * unit, 2 * unit) },
        { rectangle(-unit, -unit, split_x, 2 * unit) }
    };
    std::vector<slic3r_api::ExPolygonCollection> area_views;
    for (const ExPolygons &area : areas)
        area_views.emplace_back(area_handle(area));

    const std::vector<slic3r_api::ExtrusionAreaFragment> fragments =
        slic3r_api::MutableExtrusionEntity(entity_handle(leaf)).split_leaf_by_areas(area_views);

    REQUIRE(fragments.size() == 2);
    CHECK(fragments[0].area_index == 0);
    CHECK(fragments[1].area_index == 1);
    REQUIRE(leaf.child_count() == 2);

    const ArcPolyline &first = leaf.child(0).polyline_ref();
    const ArcPolyline &second = leaf.child(1).polyline_ref();
    CHECK(first.has_arc());
    CHECK(second.has_arc());
    CHECK(first.has_z_offset());
    CHECK(second.has_z_offset());
    CHECK(first.back() == second.front());
    CHECK(first.z_offset(first.size() - 1) == second.z_offset(0));
    CHECK(first.z_offset(first.size() - 1) > 100);
    CHECK(first.z_offset(first.size() - 1) < 900);
    CHECK(first.get_arc().back().orientation == Geometry::ArcWelder::Orientation::CCW);
    CHECK(second.get_arc().back().orientation == Geometry::ArcWelder::Orientation::CCW);
    CHECK(std::abs(first.get_arc().back().radius) == Approx(double(unit)));
    CHECK(std::abs(second.get_arc().back().radius) == Approx(double(unit)));
}

TEST_CASE("Closed extrusion splitting keeps both sides of the source seam",
          "[plugins][extrusion][split-by-areas]")
{
    const coord_t unit = scale_i(1.);
    ExtrusionPath leaf = straight_path({ Point(0, 0), Point(10 * unit, 0),
                                         Point(10 * unit, 10 * unit), Point(0, 10 * unit),
                                         Point(0, 0) });
    const coord_t split_x = 5 * unit;
    const std::vector<ExPolygons> areas = {
        { rectangle(-unit, -unit, split_x, 11 * unit) },
        { rectangle(split_x, -unit, 11 * unit, 11 * unit) }
    };
    const std::vector<const expolygon_collection_handle *> handles = area_handles(areas);
    CallbackCapture capture;
    capture.handles.reserve(3);
    capture.area_indices.reserve(3);

    REQUIRE(extrusion_split_leaf_by_areas(
                entity_handle(leaf), handles.data(), uint32_t(handles.size()), SCALED_EPSILON,
                collect_fragment, &capture) == RAW_EXTRUSION_SPLIT_STATUS_SUCCESS);

    // The first and last left-side fragments meet at the original seam, but
    // they are intentionally not merged across that seam.
    REQUIRE(leaf.child_count() == 3);
    CAPTURE(leaf.can_sort(), leaf.is_continuous(),
            leaf.child(0).last_point().x(), leaf.child(0).last_point().y(),
            leaf.child(1).first_point().x(), leaf.child(1).first_point().y(),
            leaf.child(1).last_point().x(), leaf.child(1).last_point().y(),
            leaf.child(2).first_point().x(), leaf.child(2).first_point().y());
    CHECK(capture.area_indices == std::vector<uint32_t>{ 0, 1, 0 });
    CHECK(leaf.child(0).first_point() == Point(0, 0));
    CHECK(leaf.child(2).last_point() == Point(0, 0));
    CHECK(leaf.is_continuous());
    CHECK(leaf.is_loop());
}

TEST_CASE("Invalid extrusion split inputs leave the leaf unchanged",
          "[plugins][extrusion][split-by-areas]")
{
    const coord_t unit = scale_i(1.);
    ExtrusionPath leaf = straight_path({ Point(0, 0), Point(10 * unit, 0) });
    const Points original_points = leaf.polyline_ref().to_polyline().points;

    const expolygon_collection_handle *null_area = nullptr;
    CHECK(extrusion_split_leaf_by_areas(
              entity_handle(leaf), &null_area, 1, SCALED_EPSILON, nullptr, nullptr) ==
          RAW_EXTRUSION_SPLIT_STATUS_INVALID_ARGUMENT);
    CHECK(leaf.has_polyline());
    CHECK(leaf.polyline_ref().to_polyline().points == original_points);

    const ExtrusionAttributes attributes(
        ExtrusionRole::Perimeter,
        ExtrusionFlow(0.08, 0.4f, 0.2f));
    ExtrusionPath empty_leaf(attributes, true);
    const ExPolygons valid_areas = { rectangle(-unit, -unit, 11 * unit, unit) };
    const expolygon_collection_handle *valid_handle = area_handle(valid_areas);
    CHECK(extrusion_split_leaf_by_areas(
              entity_handle(empty_leaf), &valid_handle, 1, SCALED_EPSILON, nullptr, nullptr) ==
          RAW_EXTRUSION_SPLIT_STATUS_INVALID_GEOMETRY);
    CHECK(empty_leaf.has_polyline());
    CHECK(empty_leaf.polyline_ref().empty());

    ExtrusionEntity parent(true);
    parent.append_child(std::make_unique<ExtrusionPath>(
        straight_path({ Point(0, 0), Point(10 * unit, 0) })));
    CHECK(extrusion_split_leaf_by_areas(
              entity_handle(parent), &valid_handle, 1, SCALED_EPSILON, nullptr, nullptr) ==
          RAW_EXTRUSION_SPLIT_STATUS_NOT_A_LEAF);
    CHECK(parent.child_count() == 1);

    // Clockwise contours are individually invalid for an ExPolygon area.
    ExPolygon invalid = rectangle(-unit, -unit, 11 * unit, unit);
    invalid.contour.reverse();
    const ExPolygons invalid_areas = { invalid };
    const expolygon_collection_handle *invalid_handle = area_handle(invalid_areas);
    CHECK(extrusion_split_leaf_by_areas(
              entity_handle(leaf), &invalid_handle, 1, SCALED_EPSILON, nullptr, nullptr) ==
          RAW_EXTRUSION_SPLIT_STATUS_INVALID_GEOMETRY);
    CHECK(leaf.has_polyline());
    CHECK(leaf.polyline_ref().to_polyline().points == original_points);

    const slic3r_api::ExPolygonCollection invalid_view(invalid_handle);
    CHECK_THROWS_AS(
        slic3r_api::MutableExtrusionEntity(entity_handle(leaf)).split_leaf_by_areas({ invalid_view }),
        std::invalid_argument);
    CHECK(leaf.has_polyline());
    CHECK(leaf.polyline_ref().to_polyline().points == original_points);
}
