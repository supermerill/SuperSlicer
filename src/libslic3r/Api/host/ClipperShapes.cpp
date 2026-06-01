///|/ Copyright (c) SuperSlicer 2026 Durand Rémi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/
#include "ClipperShapes.hpp"

#include <cstddef>
#include <cstdint>
#include <iterator>
#include <memory>
#include <utility>

#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/Polyline.hpp"

#include "Orchestrator.hpp"

namespace Slic3r {

static ApiClipper::ClipperShapes *store_shape(PluginStorage &plugin_storage, std::unique_ptr<ApiClipper::ClipperShapes> shape)
{
    if (!shape)
        return nullptr;
    ApiClipper::ClipperShapes *raw = shape.get();
    plugin_storage.clipper_shapes.emplace_back(std::move(shape));
    plugin_storage.generic_storage.insert(raw);
    return raw;
}

static ClipperLib::Paths concat_paths(const ApiClipper::ClipperShapes *first, const ApiClipper::ClipperShapes *second)
{
    ClipperLib::Paths out = first != nullptr ? first->to_paths() : ClipperLib::Paths{};
    if (second != nullptr) {
        ClipperLib::Paths other = second->to_paths();
        out.insert(out.end(),
                   std::make_move_iterator(other.begin()),
                   std::make_move_iterator(other.end()));
    }
    return out;
}

namespace ApiClipper {

static Polygons polygons_from_paths(const ClipperLib::Paths &paths)
{
    Polygons out;
    out.reserve(paths.size());
    for (const ClipperLib::Path &path : paths)
        out.emplace_back(path);
    return out;
}

static ExPolygons expolygons_from_polytree(const ClipperLib::PolyTree &tree)
{
    ExPolygons out;
    traverse_pt(&tree, &out);
    return out;
}

static ClipperLib::PolyTree polytree_from_paths(const ClipperLib::Paths &paths, bool do_union)
{
    ClipperLib::Clipper clipper;
    clipper.AddPaths(paths, ClipperLib::ptSubject, true);
    ClipperLib::PolyTree tree;
    const ClipperLib::PolyFillType fill_type = do_union ? ClipperLib::pftNonZero : ClipperLib::pftEvenOdd;
    clipper.Execute(ClipperLib::ctUnion, tree, fill_type, fill_type);
    return tree;
}

static Point point_from_clipper(const ClipperLib::IntPoint &point)
{
    return Point(point.x(), point.y());
}

static void merge_path_bounding_box(BoundingBox &out, const ClipperLib::Path &path)
{
    for (const ClipperLib::IntPoint &point : path)
        out.merge(point_from_clipper(point));
}

/*
Count concrete PolyTree contours without materializing them as Paths.

Clipper's PolyTree root is a container node with an empty contour, so the root
does not contribute to the result. Every real child contour, including hole
contours, is counted when it stores at least one point.
*/
template<e_ordering ordering = e_ordering::OFF>
void traverse_pt_path_count(const ClipperLib::PolyNode *tree, size_t *size_out)
{
    if (tree == nullptr || size_out == nullptr)
        return;

    if (!tree->Contour.empty())
        ++*size_out;

    foreach_node<ordering>(tree->Childs, [size_out](const ClipperLib::PolyNode *child) {
        traverse_pt_path_count<ordering>(child, size_out);
    });
}

/*
Accumulate a PolyTree bounding box by visiting each node contour directly.

The PolyTree root only owns children and usually has no contour. That is fine:
empty contours add nothing, while all real child contours, including holes and
nested islands, contribute their points to the final box.
*/
template<e_ordering ordering = e_ordering::OFF>
void traverse_pt_bounding_box(const ClipperLib::PolyNode *tree, BoundingBox *out)
{
    if (tree == nullptr || out == nullptr)
        return;

    merge_path_bounding_box(*out, tree->Contour);

    foreach_node<ordering>(tree->Childs, [out](const ClipperLib::PolyNode *child) {
        traverse_pt_bounding_box<ordering>(child, out);
    });
}

class PathListShapes final : public ClipperShapes
{
public:
    explicit PathListShapes(ClipperLib::Paths paths) : m_paths(std::move(paths)) {}

    void add_to_clipper(ClipperLib::Clipper &clipper, ClipperLib::PolyType type, bool closed) const override
    {
        clipper.AddPaths(m_paths, type, closed);
    }

    ClipperLib::Paths to_paths() const override { return m_paths; }
    Polygons to_polygons() const override { return polygons_from_paths(m_paths); }
    ExPolygons to_expolygons() const override { return expolygons_from_polytree(polytree_from_paths(m_paths, false)); }
    bool empty() const override
    {
        for (const ClipperLib::Path &path : m_paths)
            if (!path.empty())
                return false;
        return true;
    }
    uint32_t path_count() const override
    {
        uint32_t out = 0;
        for (const ClipperLib::Path &path : m_paths)
            if (!path.empty())
                ++out;
        return out;
    }
    BoundingBox bounding_box() const override
    {
        BoundingBox out;
        for (const ClipperLib::Path &path : m_paths)
            merge_path_bounding_box(out, path);
        return out;
    }

    ClipperShapes *concat(const ClipperShapes &other, Slic3r::PluginStorage &) override
    {
        ClipperLib::Paths other_paths = other.to_paths();
        m_paths.insert(m_paths.end(),
                       std::make_move_iterator(other_paths.begin()),
                       std::make_move_iterator(other_paths.end()));
        return this;
    }

private:
    ClipperLib::Paths m_paths;
};

ClipperShapes *ClipperShapes::concat(const ClipperShapes &other, Slic3r::PluginStorage &storage)
{
    return Slic3r::store_shape(storage, make_path_list_shapes(Slic3r::concat_paths(this, &other)));
}

class PolyTreeShapes final : public ClipperShapes
{
public:
    explicit PolyTreeShapes(ClipperLib::PolyTree tree) : m_tree(std::move(tree)) {}

    void add_to_clipper(ClipperLib::Clipper &clipper, ClipperLib::PolyType type, bool closed) const override
    {
        ClipperLib::Paths paths = to_paths();
        clipper.AddPaths(paths, type, closed);
    }

    ClipperLib::Paths to_paths() const override
    {
        Polygons polygons;
        traverse_pt(&m_tree, &polygons);
        ClipperLib::Paths out;
        out.reserve(polygons.size());
        for (const Polygon &polygon : polygons)
            out.emplace_back(polygon.points);
        return out;
    }

    Polygons to_polygons() const override
    {
        Polygons out;
        traverse_pt(&m_tree, &out);
        return out;
    }

    ExPolygons to_expolygons() const override
    {
        ExPolygons out;
        traverse_pt(&m_tree, &out);
        return out;
    }

    bool empty() const override { return m_tree.ChildCount() == 0; }
    uint32_t path_count() const override
    {
        size_t out = 0;
        traverse_pt_path_count(&m_tree, &out);
        return static_cast<uint32_t>(out);
    }
    BoundingBox bounding_box() const override
    {
        BoundingBox out;
        traverse_pt_bounding_box(&m_tree, &out);
        return out;
    }

private:
    ClipperLib::PolyTree m_tree;
};

class PolygonShapes final : public ClipperShapes
{
public:
    explicit PolygonShapes(const Polygon *polygon) : m_polygon(polygon) {}

    void add_to_clipper(ClipperLib::Clipper &clipper, ClipperLib::PolyType type, bool) const override
    {
        if (m_polygon != nullptr)
            clipper.AddPath(m_polygon->points, type, true);
    }

    ClipperLib::Paths to_paths() const override
    {
        return m_polygon == nullptr ? ClipperLib::Paths{} : ClipperLib::Paths{m_polygon->points};
    }

    Polygons to_polygons() const override
    {
        return m_polygon == nullptr ? Polygons{} : Polygons{*m_polygon};
    }

    ExPolygons to_expolygons() const override
    {
        return make_path_list_shapes(to_paths())->to_expolygons();
    }

    bool empty() const override { return m_polygon == nullptr || m_polygon->points.empty(); }
    uint32_t path_count() const override { return empty() ? 0 : 1; }
    BoundingBox bounding_box() const override
    {
        return m_polygon == nullptr ? BoundingBox{} : BoundingBox(m_polygon->points);
    }

private:
    const Polygon *m_polygon;
};

class PolylineShapes final : public ClipperShapes
{
public:
    explicit PolylineShapes(const Polyline *polyline) : m_polyline(polyline) {}

    void add_to_clipper(ClipperLib::Clipper &clipper, ClipperLib::PolyType type, bool) const override
    {
        if (m_polyline != nullptr)
            clipper.AddPath(m_polyline->points, type, false);
    }

    ClipperLib::Paths to_paths() const override
    {
        return m_polyline == nullptr ? ClipperLib::Paths{} : ClipperLib::Paths{m_polyline->points};
    }

    Polygons to_polygons() const override { return polygons_from_paths(to_paths()); }
    ExPolygons to_expolygons() const override { return make_path_list_shapes(to_paths())->to_expolygons(); }
    bool empty() const override { return m_polyline == nullptr || m_polyline->points.empty(); }
    uint32_t path_count() const override { return empty() ? 0 : 1; }
    BoundingBox bounding_box() const override
    {
        return m_polyline == nullptr ? BoundingBox{} : BoundingBox(m_polyline->points);
    }

private:
    const Polyline *m_polyline;
};

class MultiPointCollectionShapes final : public ClipperShapes
{
public:
    explicit MultiPointCollectionShapes(const std::vector<MultiPoint> *multipoints) : m_multipoints(multipoints) {}

    void add_to_clipper(ClipperLib::Clipper &clipper, ClipperLib::PolyType type, bool closed) const override
    {
        if (m_multipoints == nullptr)
            return;
        for (const MultiPoint &multipoint : *m_multipoints)
            clipper.AddPath(multipoint.points, type, closed);
    }

    ClipperLib::Paths to_paths() const override
    {
        ClipperLib::Paths out;
        if (m_multipoints != nullptr) {
            out.reserve(m_multipoints->size());
            for (const MultiPoint &multipoint : *m_multipoints)
                out.emplace_back(multipoint.points);
        }
        return out;
    }

    Polygons to_polygons() const override { return polygons_from_paths(to_paths()); }
    ExPolygons to_expolygons() const override { return make_path_list_shapes(to_paths())->to_expolygons(); }
    bool empty() const override
    {
        if (m_multipoints == nullptr)
            return true;
        for (const MultiPoint &multipoint : *m_multipoints)
            if (!multipoint.points.empty())
                return false;
        return true;
    }
    uint32_t path_count() const override
    {
        if (m_multipoints == nullptr)
            return 0;

        uint32_t out = 0;
        for (const MultiPoint &multipoint : *m_multipoints)
            if (!multipoint.points.empty())
                ++out;
        return out;
    }
    BoundingBox bounding_box() const override
    {
        BoundingBox out;
        if (m_multipoints != nullptr) {
            for (const MultiPoint &multipoint : *m_multipoints)
                out.merge(multipoint.points);
        }
        return out;
    }

private:
    const std::vector<MultiPoint> *m_multipoints;
};

class ExPolygonShapes final : public ClipperShapes
{
public:
    explicit ExPolygonShapes(const ExPolygon *expolygon) : m_expolygon(expolygon) {}

    void add_to_clipper(ClipperLib::Clipper &clipper, ClipperLib::PolyType type, bool) const override
    {
        if (m_expolygon == nullptr)
            return;
        clipper.AddPath(m_expolygon->contour.points, type, true);
        for (const Polygon &hole : m_expolygon->holes)
            clipper.AddPath(hole.points, type, true);
    }

    ClipperLib::Paths to_paths() const override
    {
        ClipperLib::Paths out;
        if (m_expolygon != nullptr) {
            out.reserve(m_expolygon->holes.size() + 1);
            out.emplace_back(m_expolygon->contour.points);
            for (const Polygon &hole : m_expolygon->holes)
                out.emplace_back(hole.points);
        }
        return out;
    }

    Polygons to_polygons() const override { return polygons_from_paths(to_paths()); }
    ExPolygons to_expolygons() const override
    {
        return m_expolygon == nullptr ? ExPolygons{} : ExPolygons{*m_expolygon};
    }

    bool empty() const override { return m_expolygon == nullptr || m_expolygon->contour.points.empty(); }
    uint32_t path_count() const override
    {
        if (m_expolygon == nullptr)
            return 0;

        uint32_t out = m_expolygon->contour.points.empty() ? 0 : 1;
        for (const Polygon &hole : m_expolygon->holes)
            if (!hole.points.empty())
                ++out;
        return out;
    }
    BoundingBox bounding_box() const override
    {
        BoundingBox out;
        if (m_expolygon != nullptr) {
            out.merge(m_expolygon->contour.points);
            for (const Polygon &hole : m_expolygon->holes)
                out.merge(hole.points);
        }
        return out;
    }

private:
    const ExPolygon *m_expolygon;
};

class ExPolygonsShapes final : public ClipperShapes
{
public:
    explicit ExPolygonsShapes(const ExPolygons *expolygons) : m_expolygons(expolygons) {}

    void add_to_clipper(ClipperLib::Clipper &clipper, ClipperLib::PolyType type, bool) const override
    {
        if (m_expolygons == nullptr)
            return;
        for (const ExPolygon &expolygon : *m_expolygons) {
            clipper.AddPath(expolygon.contour.points, type, true);
            for (const Polygon &hole : expolygon.holes)
                clipper.AddPath(hole.points, type, true);
        }
    }

    ClipperLib::Paths to_paths() const override
    {
        ClipperLib::Paths out;
        if (m_expolygons != nullptr) {
            for (const ExPolygon &expolygon : *m_expolygons) {
                out.emplace_back(expolygon.contour.points);
                for (const Polygon &hole : expolygon.holes)
                    out.emplace_back(hole.points);
            }
        }
        return out;
    }

    Polygons to_polygons() const override { return polygons_from_paths(to_paths()); }
    ExPolygons to_expolygons() const override
    {
        return m_expolygons == nullptr ? ExPolygons{} : *m_expolygons;
    }

    bool empty() const override
    {
        if (m_expolygons == nullptr)
            return true;
        for (const ExPolygon &expolygon : *m_expolygons)
            if (!expolygon.contour.points.empty())
                return false;
        return true;
    }
    uint32_t path_count() const override
    {
        if (m_expolygons == nullptr)
            return 0;

        uint32_t out = 0;
        for (const ExPolygon &expolygon : *m_expolygons) {
            if (!expolygon.contour.points.empty())
                ++out;
            for (const Polygon &hole : expolygon.holes)
                if (!hole.points.empty())
                    ++out;
        }
        return out;
    }
    BoundingBox bounding_box() const override
    {
        BoundingBox out;
        if (m_expolygons != nullptr) {
            for (const ExPolygon &expolygon : *m_expolygons) {
                out.merge(expolygon.contour.points);
                for (const Polygon &hole : expolygon.holes)
                    out.merge(hole.points);
            }
        }
        return out;
    }

private:
    const ExPolygons *m_expolygons;
};

std::unique_ptr<ClipperShapes> make_path_list_shapes(ClipperLib::Paths paths)
{
    return std::make_unique<PathListShapes>(std::move(paths));
}

std::unique_ptr<ClipperShapes> make_polytree_shapes(ClipperLib::PolyTree tree)
{
    return std::make_unique<PolyTreeShapes>(std::move(tree));
}

std::unique_ptr<ClipperShapes> make_polygon_shapes(const Polygon *polygon)
{
    return std::make_unique<PolygonShapes>(polygon);
}

std::unique_ptr<ClipperShapes> make_polyline_shapes(const Polyline *polyline)
{
    return std::make_unique<PolylineShapes>(polyline);
}

std::unique_ptr<ClipperShapes> make_multipoint_collection_shapes(const std::vector<MultiPoint> *multipoints)
{
    return std::make_unique<MultiPointCollectionShapes>(multipoints);
}

std::unique_ptr<ClipperShapes> make_expolygon_shapes(const ExPolygon *expolygon)
{
    return std::make_unique<ExPolygonShapes>(expolygon);
}

std::unique_ptr<ClipperShapes> make_expolygons_shapes(const ExPolygons *expolygons)
{
    return std::make_unique<ExPolygonsShapes>(expolygons);
}

} // namespace ApiClipper
} // namespace Slic3r
