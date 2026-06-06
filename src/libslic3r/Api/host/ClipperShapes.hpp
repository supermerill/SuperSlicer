///|/ Copyright (c) SuperSlicer 2026 Durand Rémi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/
#ifndef slic3r_Api_ClipperShapes_hpp_
#define slic3r_Api_ClipperShapes_hpp_

#include <cstdint>
#include <memory>

#include "libslic3r/ExPolygon.hpp"
#include "libslic3r/Polygon.hpp"

#include <clipper/clipper.hpp>

namespace Slic3r {
class BoundingBox;
class MultiPoint;
class PluginStorage;
class Polyline;
}

namespace Slic3r::ApiClipper {

/*
Internal C++ interface behind the public clipper_shapes_handle.

Each implementation knows how to expose one geometry representation as Clipper
paths. Some implementations are lightweight adapters over existing Slic3r
objects, while others own Clipper output such as Paths or PolyTree.

The public ABI never exposes this type directly. It only sees the opaque
clipper_shapes_handle. Keeping the polymorphism here lets the ABI offer one
small operation set instead of duplicating every boolean operation for Polygon,
Polygons, ExPolygon and ExPolygons.
*/
class ClipperShapes
{
public:
    virtual ~ClipperShapes() = default;

    /*
    Add all paths represented by this shape to a Clipper instance.

    type is ptSubject or ptClip depending on the boolean operation side.
    closed is forwarded to Clipper::AddPath(s) and controls whether the paths
    are treated as closed polygons or open polylines.
    */
    virtual void add_to_clipper(ClipperLib::Clipper &clipper, ClipperLib::PolyType type, bool closed) const = 0;

    /*
    Convert to raw Clipper paths. This is mainly used by ClipperOffset, which
    operates through ClipperOffset::AddPaths rather than Clipper::AddPaths.
    */
    virtual ClipperLib::Paths to_paths() const = 0;

    /*
    Materialize as Slic3r polygons. Holes are not grouped with contours here;
    callers that need island/hole structure should use to_expolygons().
    */
    virtual Polygons to_polygons() const = 0;

    /*
    Materialize as Slic3r ExPolygons, grouping contours and holes where the
    underlying representation contains enough hierarchy, for example PolyTree.
    */
    virtual ExPolygons to_expolygons() const = 0;

    /*
    Fast emptiness test for callers that only need to know whether an operation
    produced usable geometry. Implementations should answer from their native
    representation when possible instead of materializing Polygons/ExPolygons.
    */
    virtual bool empty() const = 0;

    /*
    Count non-empty raw Clipper paths represented by this shape. This is not an
    ExPolygon count: a contour counts as one path and each non-empty hole counts
    as one path too.
    */
    virtual uint32_t path_count() const = 0;

    /*
    Return the axis-aligned box of every point represented by this shape. This
    is intentionally a raw point extent, not an ExPolygon extent: holes and open
    paths contribute points exactly like contours.
    */
    virtual BoundingBox bounding_box() const = 0;

    /*
    Concatenate with another shape for the "append then maybe union later" use
    case.

    Default behaviour is to allocate a new PathListShapes in storage containing
    the raw paths of both operands. Implementations that already own mutable
    raw path storage, such as PathListShapes, may override this and append
    directly into themselves, returning this.
    */
    virtual ClipperShapes *concat(const ClipperShapes &other, Slic3r::PluginStorage &storage);
};

std::unique_ptr<ClipperShapes> make_path_list_shapes(ClipperLib::Paths paths);
std::unique_ptr<ClipperShapes> make_polytree_shapes(ClipperLib::PolyTree tree);
std::unique_ptr<ClipperShapes> make_polyline_shapes(const Polyline *polyline);
std::unique_ptr<ClipperShapes> make_polygon_shapes(const Polygon *polygon);
std::unique_ptr<ClipperShapes> make_polygons_shapes(const Polygons *polygons);
std::unique_ptr<ClipperShapes> make_expolygon_shapes(const ExPolygon *expolygon);
std::unique_ptr<ClipperShapes> make_expolygons_shapes(const ExPolygons *expolygons);

} // namespace Slic3r::ApiClipper

#endif
