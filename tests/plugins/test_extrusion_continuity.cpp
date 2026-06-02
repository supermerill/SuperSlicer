#include <catch2/catch.hpp>

#include "libslic3r/ExtrusionEntity.hpp"
#include "libslic3r/ExtrusionEntityCollection.hpp"

namespace {

Slic3r::ExtrusionPath path_between(const Slic3r::Point &from, const Slic3r::Point &to)
{
    Slic3r::ExtrusionPath path(
        Slic3r::ExtrusionAttributes(
            Slic3r::ExtrusionRole::Perimeter,
            Slic3r::ExtrusionFlow(0.1, 0.4f, 0.2f)),
        nullptr,
        true);
    path.polyline().append(from);
    path.polyline().append(to);
    return path;
}

} // namespace

TEST_CASE("Extrusion continuity is computed from tree order", "[plugins][extrusion][continuity]")
{
    const Slic3r::Point p0(0, 0);
    const Slic3r::Point p1(100, 0);
    const Slic3r::Point p2(200, 0);
    const Slic3r::Point p3(300, 0);

    SECTION("empty entities and leaf polylines are continuous")
    {
        // A leaf has no child order to validate. It is therefore continuous by
        // nature unless a caller explicitly marks it sortable, which is an
        // invalid state for local-polyline entities and rejected by the C API.
        Slic3r::ExtrusionEntity empty(true);
        Slic3r::ExtrusionPath leaf = path_between(p0, p1);

        CHECK(empty.is_continuous());
        CHECK(leaf.is_continuous());
    }

    SECTION("sortable collections are never continuous")
    {
        // Sorting gives path planners permission to reorder children. The
        // current children happen to touch, but the collection is not a forced
        // continuous path while m_can_sort is enabled.
        Slic3r::ExtrusionEntityCollection sortable;
        sortable.append(path_between(p0, p1));
        sortable.append(path_between(p1, p2));

        CHECK(sortable.can_sort());
        CHECK_FALSE(sortable.is_continuous());
    }

    SECTION("non-sortable collections are continuous when children touch")
    {
        // Disabling sorting makes the current child order authoritative. The
        // continuity check then walks non-empty children and verifies that each
        // child ends exactly where the next one starts.
        Slic3r::ExtrusionEntityCollection ordered(false, true);
        ordered.append(path_between(p0, p1));
        ordered.append(path_between(p1, p2));

        CHECK_FALSE(ordered.can_sort());
        CHECK(ordered.is_continuous());
    }

    SECTION("a gap between two children breaks continuity")
    {
        Slic3r::ExtrusionEntityCollection ordered(false, true);
        ordered.append(path_between(p0, p1));
        ordered.append(path_between(p2, p3));

        CHECK_FALSE(ordered.is_continuous());
    }

    SECTION("a continuous closed generic collection is a loop")
    {
        Slic3r::ExtrusionEntityCollection loop(false, true);
        loop.append(path_between(p0, p1));
        loop.append(path_between(p1, p0));

        CHECK(loop.is_continuous());
        CHECK(loop.is_loop());
    }

    SECTION("a non-continuous child makes the parent non-continuous")
    {
        // Continuity is recursive. A parent cannot become continuous just
        // because its child collection has matching first/back points if that
        // child remains sortable internally.
        Slic3r::ExtrusionEntityCollection sortable_child;
        sortable_child.append(path_between(p0, p1));
        sortable_child.append(path_between(p1, p2));

        Slic3r::ExtrusionEntityCollection parent(false, true);
        parent.append(sortable_child);

        CHECK_FALSE(sortable_child.is_continuous());
        CHECK_FALSE(parent.is_continuous());
    }
}
