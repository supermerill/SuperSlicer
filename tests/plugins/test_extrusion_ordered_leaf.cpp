///|/ Copyright (c) SuperSlicer 2026 Durand Remi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/

/*
Tests for ordered boundary leaves in the unified extrusion tree.

The helper keeps the caller's outer handle stable while either inserting into
an already fixed collection or preserving the previous logical entity below a
new wrapper. These tests focus on ownership, inherited-property placement and
the strong no-mutation contract of rejected C calls.
*/

#include <catch2/catch.hpp>

#include <memory>
#include <string>

#include "libslic3r/Api/plugin/c/slic3r_extrusion_entity.h"
#include "libslic3r/Api/plugin/cpp/ExtrusionViews.hpp"
#include "libslic3r/ExtrusionEntity.hpp"

namespace {

Slic3r::ExtrusionEntityUPtr empty_child();
extrusion_entity_handle *entity_handle(Slic3r::ExtrusionEntity &entity);
slic3r_api::MutableExtrusionEntity mutable_view(Slic3r::ExtrusionEntity &entity);

Slic3r::ExtrusionEntityUPtr empty_child()
{
    return std::make_unique<Slic3r::ExtrusionEntity>(false);
}

extrusion_entity_handle *entity_handle(Slic3r::ExtrusionEntity &entity)
{
    return reinterpret_cast<extrusion_entity_handle *>(&entity);
}

slic3r_api::MutableExtrusionEntity mutable_view(Slic3r::ExtrusionEntity &entity)
{
    return slic3r_api::MutableExtrusionEntity(entity_handle(entity));
}

} // namespace

TEST_CASE("Ordered leaves insert directly into fixed parents",
          "[plugins][extrusion][ordered-leaf]")
{
    Slic3r::ExtrusionEntity::Children children;
    children.emplace_back(empty_child());
    children.emplace_back(empty_child());
    Slic3r::ExtrusionEntity root(std::move(children), false, false, true);
    root.add_property(Slic3r::ExtrusionPropertySpeed(35.f, 700.f));

    Slic3r::ExtrusionEntity *first_original = &root.child(0);
    Slic3r::ExtrusionEntity *second_original = &root.child(1);
    slic3r_api::MutableExtrusionEntity inserted = mutable_view(root).emplace_ordered_leaf(
        slic3r_api::OrderedLeafPosition::Before,
        slic3r_api::ExistingPropertyPlacement::KeepOnParent);

    REQUIRE(inserted.valid());
    REQUIRE(root.child_count() == 3);
    CHECK(&root.child(0) == reinterpret_cast<Slic3r::ExtrusionEntity *>(inserted.mutable_handle()));
    CHECK(&root.child(1) == first_original);
    CHECK(&root.child(2) == second_original);
    CHECK(root.get_property<Slic3r::ExtrusionPropertySpeed>() != nullptr);
    CHECK_FALSE(root.can_sort());
    CHECK_FALSE(root.can_reverse());
    CHECK((inserted.flags() & RAW_EXTRUSION_FLAG_SORTABLE) == 0);
    CHECK((inserted.flags() & RAW_EXTRUSION_FLAG_REVERSIBLE) == 0);
}

TEST_CASE("Ordered leaves wrap content whose ordering may change",
          "[plugins][extrusion][ordered-leaf]")
{
    Slic3r::ExtrusionEntity::Children children;
    children.emplace_back(empty_child());
    Slic3r::ExtrusionEntity root(std::move(children), true, true, false);
    root.add_property(Slic3r::ExtrusionPropertySpeed(20.f, 400.f));
    Slic3r::ExtrusionEntity *original_child = &root.child(0);

    slic3r_api::MutableExtrusionEntity inserted = mutable_view(root).emplace_ordered_leaf(
        slic3r_api::OrderedLeafPosition::After,
        slic3r_api::ExistingPropertyPlacement::KeepOnParent);

    REQUIRE(inserted.valid());
    REQUIRE(root.child_count() == 2);
    const Slic3r::ExtrusionEntity &preserved = root.child(0);
    CHECK(&root.child(1) == reinterpret_cast<Slic3r::ExtrusionEntity *>(inserted.mutable_handle()));
    REQUIRE(preserved.child_count() == 1);
    CHECK(&preserved.child(0) == original_child);
    CHECK(preserved.can_sort());
    CHECK(preserved.can_reverse());
    CHECK(preserved.get_property<Slic3r::ExtrusionPropertySpeed>() == nullptr);
    CHECK(root.get_property<Slic3r::ExtrusionPropertySpeed>() != nullptr);
    CHECK_FALSE(root.can_sort());
    CHECK_FALSE(root.can_reverse());
}

TEST_CASE("Moving existing properties always creates a preserved-content wrapper",
          "[plugins][extrusion][ordered-leaf]")
{
    Slic3r::ExtrusionEntity::Children children;
    children.emplace_back(empty_child());
    Slic3r::ExtrusionEntity root(std::move(children), false, false, true);
    Slic3r::ExtrusionEntity *original_child = &root.child(0);

    slic3r_api::MutableExtrusionEntity inserted = mutable_view(root).emplace_ordered_leaf(
        slic3r_api::OrderedLeafPosition::Before,
        slic3r_api::ExistingPropertyPlacement::MoveWithExistingContent);

    REQUIRE(inserted.valid());
    REQUIRE(root.child_count() == 2);
    REQUIRE(root.child(1).child_count() == 1);
    CHECK(&root.child(1).child(0) == original_child);
    CHECK(&root.child(0) == reinterpret_cast<Slic3r::ExtrusionEntity *>(inserted.mutable_handle()));
}

TEST_CASE("Ordered leaf wrapping moves property resources with leaf content",
          "[plugins][extrusion][ordered-leaf]")
{
    const Slic3r::coord_t unit = 1000;
    Slic3r::ArcPolyline polyline;
    polyline.append(Slic3r::Point(unit, 0));
    polyline.append(Slic3r::Geometry::ArcWelder::Segment(
        Slic3r::Point(0, unit),
        float(unit),
        Slic3r::Geometry::ArcWelder::Orientation::CCW));
    polyline.set_z_offset(0, 100);
    polyline.set_z_offset(1, 900);
    Slic3r::ExtrusionEntity root(false, std::move(polyline));
    Slic3r::ExtrusionPropertyCustomGcode &custom = root.add_property(
        Slic3r::ExtrusionPropertyCustomGcodeText("M117 preserved custom G-code"));
    REQUIRE(root.custom_gcode_string(custom) == "M117 preserved custom G-code");

    slic3r_api::MutableExtrusionEntity inserted = mutable_view(root).emplace_ordered_leaf(
        slic3r_api::OrderedLeafPosition::After,
        slic3r_api::ExistingPropertyPlacement::MoveWithExistingContent);

    REQUIRE(inserted.valid());
    REQUIRE(root.child_count() == 2);
    CHECK(root.get_property<Slic3r::ExtrusionPropertyCustomGcode>() == nullptr);
    const Slic3r::ExtrusionEntity &preserved = root.child(0);
    const Slic3r::ExtrusionPropertyCustomGcode *preserved_custom =
        preserved.get_property<Slic3r::ExtrusionPropertyCustomGcode>();
    REQUIRE(preserved_custom != nullptr);
    CHECK(preserved.custom_gcode_string(*preserved_custom) == "M117 preserved custom G-code");
    CHECK(preserved.has_polyline());
    CHECK(preserved.polyline_ref().size() == 2);
    CHECK(preserved.polyline_ref().has_arc());
    CHECK(preserved.polyline_ref().has_z_offset());
    CHECK(preserved.polyline_ref().z_offset(0) == 100);
    CHECK(preserved.polyline_ref().z_offset(1) == 900);
    CHECK(preserved.polyline_ref().get_arc().back().orientation ==
          Slic3r::Geometry::ArcWelder::Orientation::CCW);
    CHECK(&root.child(1) == reinterpret_cast<Slic3r::ExtrusionEntity *>(inserted.mutable_handle()));
}

TEST_CASE("Invalid ordered leaf arguments leave the entity unchanged",
          "[plugins][extrusion][ordered-leaf]")
{
    Slic3r::ExtrusionEntity root(false);
    root.add_property(Slic3r::ExtrusionPropertySpeed(12.f, 300.f));

    extrusion_entity_handle *result = extrusion_emplace_ordered_leaf(
        entity_handle(root),
        static_cast<raw_extrusion_ordered_leaf_position>(99),
        RAW_EXTRUSION_EXISTING_PROPERTIES_KEEP_ON_PARENT);

    CHECK(result == nullptr);
    CHECK(root.is_leaf());
    CHECK(root.child_count() == 0);
    CHECK(root.get_property<Slic3r::ExtrusionPropertySpeed>() != nullptr);
}
