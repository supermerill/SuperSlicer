///|/ Copyright (c) SuperSlicer 2026 Durand R?mi @supermerill
///|/ Copyright (c) Prusa Research 2023 Lukáš Matěna @lukasmatena, Enrico Turri @enricoturri1966
///|/ Copyright (c) BambuStudio 2023 manch1n @manch1n
///|/
///|/ PrusaSlicer is released under the terms of the AGPLv3 or higher
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/

#include "ConflictChecker.hpp"

#include <atomic>
#include <functional>
#include <map>

#include <oneapi/tbb/concurrent_vector.h>
#include <oneapi/tbb/parallel_for.h>

#include "libslic3r/Layer.hpp"
#include "libslic3r/PointUtils.hpp"
#include "libslic3r/PrintObject.hpp"

namespace Slic3r {

namespace RasterizationImpl {
using IndexPair = std::pair<int64_t, int64_t>;
using Grids     = std::vector<IndexPair>;

inline constexpr int64_t RasteXDistance = int64_t(scale_i(1));
inline constexpr int64_t RasteYDistance = int64_t(scale_i(1));

inline IndexPair point_map_grid_index(const Point &pt, int64_t xdist, int64_t ydist)
{
    auto x = pt.x() / xdist;
    auto y = pt.y() / ydist;
    return std::make_pair(x, y);
}

inline bool nearly_equal(const Point &p1, const Point &p2) { return std::abs(p1.x() - p2.x()) < SCALED_EPSILON && std::abs(p1.y() - p2.y()) < SCALED_EPSILON; }

inline Grids line_rasterization(const Line &line, int64_t xdist = RasteXDistance, int64_t ydist = RasteYDistance)
{
    Grids     res;
    Point     rayStart     = line.a;
    Point     rayEnd       = line.b;
    IndexPair currentVoxel = point_map_grid_index(rayStart, xdist, ydist);
    IndexPair lastVoxel    = point_map_grid_index(rayEnd, xdist, ydist);

    Point ray = rayEnd - rayStart;

    double stepX = ray.x() >= 0 ? 1 : -1;
    double stepY = ray.y() >= 0 ? 1 : -1;

    double nextVoxelBoundaryX = (currentVoxel.first + stepX) * xdist;
    double nextVoxelBoundaryY = (currentVoxel.second + stepY) * ydist;

    if (stepX < 0) { nextVoxelBoundaryX += xdist; }
    if (stepY < 0) { nextVoxelBoundaryY += ydist; }

    double tMaxX = ray.x() != 0 ? (nextVoxelBoundaryX - rayStart.x()) / ray.x() : DBL_MAX;
    double tMaxY = ray.y() != 0 ? (nextVoxelBoundaryY - rayStart.y()) / ray.y() : DBL_MAX;

    double tDeltaX = ray.x() != 0 ? static_cast<double>(xdist) / ray.x() * stepX : DBL_MAX;
    double tDeltaY = ray.y() != 0 ? static_cast<double>(ydist) / ray.y() * stepY : DBL_MAX;

    res.push_back(currentVoxel);

    double tx = tMaxX;
    double ty = tMaxY;

    while (lastVoxel != currentVoxel) {
        if (lastVoxel.first == currentVoxel.first) {
            for (int64_t i = currentVoxel.second; i != lastVoxel.second; i += (int64_t) stepY) {
                currentVoxel.second += (int64_t) stepY;
                res.push_back(currentVoxel);
            }
            break;
        }
        if (lastVoxel.second == currentVoxel.second) {
            for (int64_t i = currentVoxel.first; i != lastVoxel.first; i += (int64_t) stepX) {
                currentVoxel.first += (int64_t) stepX;
                res.push_back(currentVoxel);
            }
            break;
        }

        if (tx < ty) {
            currentVoxel.first += (int64_t) stepX;
            tx += tDeltaX;
        } else {
            currentVoxel.second += (int64_t) stepY;
            ty += tDeltaY;
        }
        res.push_back(currentVoxel);
        if (res.size() >= 100000) { // bug
            assert(0);
        }
    }

    return res;
}
} // namespace RasterizationImpl



static std::vector<ExtrusionPaths> getFakeExtrusionPathsFromWipeTower(const WipeTowerData& wtd)
{
    float h = wtd.height;
    float lh = wtd.first_layer_height;
    coord_t d = scale_i(wtd.depth);
    coord_t w = scale_i(wtd.width);
    coord_t bd = scale_i(wtd.brim_width);
    Point minCorner = { -wtd.brim_width, -wtd.brim_width };
    Point maxCorner = { minCorner.x() + w + bd, minCorner.y() + d + bd };
    float width = wtd.width;
    float depth = wtd.depth;
    float height = wtd.height;
    float cone_angle = wtd.cone_angle;
    const auto& z_and_depth_pairs = wtd.z_and_depth_pairs;

    const auto [cone_base_R, cone_scale_x] = WipeTower::get_wipe_tower_cone_base(width, height, depth, cone_angle);

    std::vector<ExtrusionPaths> paths;
    for (float hh = 0.f; hh < h; hh += lh) {
        
        if (hh != 0.f) {
            // The wipe tower may be getting smaller. Find the depth for this layer.
            size_t i = 0;
            for (i=0; i<z_and_depth_pairs.size()-1; ++i)
                if (hh >= z_and_depth_pairs[i].first && hh < z_and_depth_pairs[i+1].first)
                    break;
            d = scale_i(z_and_depth_pairs[i].second);
            minCorner = {0, -d/2 + scale_i(z_and_depth_pairs.front().second/2.f)};
            maxCorner = { minCorner.x() + w, minCorner.y() + d };
        }


        ExtrusionPath path(ArcPolyline(Points{minCorner, {maxCorner.x(), minCorner.y()}, maxCorner, {minCorner.x(), maxCorner.y()}, minCorner }),
            ExtrusionAttributes{ ExtrusionRole::WipeTower, ExtrusionFlow{ 0.0, 0.0, lh } }, nullptr);
        paths.push_back({ path });

        // We added the border, now add several parallel lines so we can detect an object that is fully inside the tower.
        // For now, simply use fixed spacing of 3mm.
        for (coord_t y = minCorner.y() + scale_i(3.); y < maxCorner.y(); y += scale_i(3.)) {
            path.polyline() = ArcPolyline(Points{ {minCorner.x(), y}, {maxCorner.x(), y} });
            assert(path.polyline().is_valid());
            paths.back().emplace_back(path);
        }

        // And of course the stabilization cone and its base...
        if (cone_base_R > 0.) {
            path.polyline().clear();
            double r = cone_base_R * (1 - hh / height);
            for (double alpha = 0; alpha < 2.01 * M_PI; alpha += 2 * M_PI / 20.) {
                Point point = Point::new_scale(width / 2. + r * std::cos(alpha) / cone_scale_x,
                                               depth / 2. + r * std::sin(alpha));
                if (path.polyline().empty() || !point.coincides_with_epsilon(path.polyline().front())) {
                    path.polyline().append(std::move(point));
                }
            }
            if (path.size() > 1) {
                assert(path.polyline().is_valid());
                paths.back().emplace_back(path);
            }
            if (hh == 0.f) { // Cone brim.
                for (float bw = wtd.brim_width; bw > 0.f; bw -= 3.f) {
                    path.polyline().clear();
                    for (double alpha = 0; alpha < 2.01 * M_PI; alpha += 2 * M_PI / 20.) {
                        // see load_wipe_tower_preview, where the same is a bit clearer
                        Point point = Point::new_scale(width / 2. + cone_base_R * std::cos(alpha) / cone_scale_x * (1. + cone_scale_x * bw / cone_base_R),
                                                       depth / 2. + cone_base_R * std::sin(alpha) * (1. + bw / cone_base_R));
                        if (path.polyline().empty() || !point.coincides_with_epsilon(path.polyline().front())) {
                            path.polyline().append(std::move(point));
                        }
                    }
                    if (path.size() > 1) {
                        assert(path.polyline().is_valid());
                        paths.back().emplace_back(path);
                    }
                }
            }
        }

        // Only the first layer has brim.
        if (hh == 0.f) {
            minCorner = minCorner + Point(bd, bd);
            maxCorner = maxCorner - Point(bd, bd);
        }
    }

    // Rotate and translate the tower into the final position.
    for (ExtrusionPaths& ps : paths) {
        for (ExtrusionPath& p : ps) {
            p.polyline().rotate(Geometry::deg2rad(wtd.rotation_angle));
            p.polyline().translate(Vector(scale_i(wtd.position.x()), scale_i(wtd.position.y())));
        }
    }

    return paths;
}



void LinesBucketQueue::emplace_back_bucket(std::vector<ExtrusionPaths> &&paths, const void *objPtr, Points offsets)
{
    if (_objsPtrToId.find(objPtr) == _objsPtrToId.end()) {
        _objsPtrToId.insert({objPtr, _objsPtrToId.size()});
        _idToObjsPtr.insert({_objsPtrToId.size() - 1, objPtr});
    }
    _buckets.emplace_back(std::move(paths), _objsPtrToId[objPtr], offsets);
}

void LinesBucketQueue::build_queue()
{
    assert(_pq.empty());
    for (LinesBucket &bucket : _buckets)
        _pq.push(&bucket);
}

double LinesBucketQueue::removeLowests()
{
    auto lowest = _pq.top();
    _pq.pop();
    double                     curHeight = lowest->curHeight();
    std::vector<LinesBucket *> lowests;
    lowests.push_back(lowest);

    while (_pq.empty() == false && std::abs(_pq.top()->curHeight() - lowest->curHeight()) < EPSILON) {
        lowests.push_back(_pq.top());
        _pq.pop();
    }

    for (LinesBucket *bp : lowests) {
        bp->raise();
        if (bp->valid()) { _pq.push(bp); }
    }
    return curHeight;
}

LineWithIDs LinesBucketQueue::getCurLines() const
{
    LineWithIDs lines;
    for (const LinesBucket &bucket : _buckets) {
        if (bucket.valid()) {
            LineWithIDs tmpLines = bucket.curLines();
            lines.insert(lines.end(), tmpLines.begin(), tmpLines.end());
        }
    }
    return lines;
}

void getExtrusionPathsFromEntity(const ExtrusionEntityCollection *entity, ExtrusionPaths &paths)
{
    std::function<void(const ExtrusionEntityCollection *, ExtrusionPaths &)> getExtrusionPathImpl = [&](const ExtrusionEntityCollection *entity, ExtrusionPaths &paths) {
        for (auto entityPtr : entity->entities()) {
            if (const ExtrusionEntityCollection *collection = dynamic_cast<ExtrusionEntityCollection *>(entityPtr)) {
                getExtrusionPathImpl(collection, paths);
            } else if (const ExtrusionPath *path = dynamic_cast<ExtrusionPath *>(entityPtr)) {
                paths.push_back(*path);
            } else if (const ExtrusionMultiPath *multipath = dynamic_cast<ExtrusionMultiPath *>(entityPtr)) {
                for (const ExtrusionPath &path : multipath->paths()) { paths.push_back(path); }
            } else if (const ExtrusionLoop *loop = dynamic_cast<ExtrusionLoop *>(entityPtr)) {
                for (const ExtrusionPath &path : loop->paths()) { paths.push_back(path); }
            }
        }
    };
    getExtrusionPathImpl(entity, paths);
}

ExtrusionPaths getExtrusionPathsFromLayer(LayerSliceIslandCRefs layer_islands)
{
    ExtrusionPaths paths;
    for (const LayerSliceIsland &layer_island_ptr : layer_islands) {
        for (const LayerRegionIsland &region_island_ptr : layer_island_ptr.regions_islands()) {
            if (region_island_ptr.has_extrusion(LayerRegionIsland::PERIMETERS)) {
                getExtrusionPathsFromEntity(&region_island_ptr.extrusion(LayerRegionIsland::PERIMETERS), paths);
                if (region_island_ptr.has_extrusion(LayerRegionIsland::INFILLS)) {
                    getExtrusionPathsFromEntity(&region_island_ptr.extrusion(LayerRegionIsland::INFILLS), paths);
                }
            }
        }
    }
    return paths;
}

ExtrusionPaths getExtrusionPathsFromSupportLayer(const Layer *layer)
{
    assert(layer);
    ExtrusionPaths paths;
    if (layer->get_property<LayerSupportProperty>() == nullptr)
        return paths;
    for (const LayerSliceIsland &island : layer->islands()) {
        for (const LayerRegionIsland &region_island : island.regions_islands()) {
            if (region_island.has_extrusion(LayerRegionIsland::SUPPORT)) {
                getExtrusionPathsFromEntity(&region_island.extrusion(LayerRegionIsland::SUPPORT), paths);
            }
            if (region_island.has_extrusion(LayerRegionIsland::SUPPORT_INTERFACE)) {
                getExtrusionPathsFromEntity(&region_island.extrusion(LayerRegionIsland::SUPPORT_INTERFACE), paths);
            }
        }
    }
    return paths;
}

std::pair<std::vector<ExtrusionPaths>, std::vector<ExtrusionPaths>> getAllLayersExtrusionPathsFromObject(const PrintObject *obj)
{
    std::vector<ExtrusionPaths> objPaths, supportPaths;

    for (const Layer &layerPtr : obj->layers()) { objPaths.push_back(getExtrusionPathsFromLayer(layerPtr.islands())); }

    for (const Layer &supportLayerPtr : obj->auxiliary_layers()) {
        if (supportLayerPtr.get_property<LayerSupportProperty>() != nullptr)
            supportPaths.push_back(getExtrusionPathsFromSupportLayer(&supportLayerPtr));
    }

    return {std::move(objPaths), std::move(supportPaths)};
}

ConflictComputeOpt ConflictChecker::find_inter_of_lines(const LineWithIDs &lines)
{
    using namespace RasterizationImpl;
    std::map<IndexPair, std::vector<int>> indexToLine;

    for (int i = 0; i < (int)lines.size(); ++i) {
        const LineWithID &l1      = lines[i];
        auto              indexes = line_rasterization(l1._line);
        for (auto index : indexes) {
            const auto &possibleIntersectIdxs = indexToLine[index];
            for (auto possibleIntersectIdx : possibleIntersectIdxs) {
                const LineWithID &l2 = lines[possibleIntersectIdx];
                if (auto interRes = line_intersect(l1, l2); interRes.has_value()) { return interRes; }
            }
            indexToLine[index].push_back(i);
        }
    }
    return {};
}

ConflictResultOpt ConflictChecker::find_inter_of_lines_in_diff_objs(SpanOfConstPtrs<PrintObject> objs,
                                                                    const WipeTowerData& wipe_tower_data) // find the first intersection point of lines in different objects
{
    if (objs.empty() || (objs.size() == 1 && objs.front()->instances().size() == 1 && wipe_tower_data.number_of_toolchanges <= 0)) { return {}; }

    // The code ported from BS uses void* to identify objects...
    // Let's use the address of this variable to represent the wipe tower.
    int wtptr = 0;

    LinesBucketQueue conflictQueue;
    if (! wipe_tower_data.z_and_depth_pairs.empty() && wipe_tower_data.tool_changes.size() > 0) {
        // The wipe tower is being generated.
        const Vec2d plate_origin = Vec2d::Zero();
        std::vector<ExtrusionPaths> wtpaths = getFakeExtrusionPathsFromWipeTower(wipe_tower_data);
        conflictQueue.emplace_back_bucket(std::move(wtpaths), &wtptr, Points{Point(plate_origin)});
    }
    for (const PrintObject *obj : objs) {
        std::pair<std::vector<ExtrusionPaths>, std::vector<ExtrusionPaths>> layers = getAllLayersExtrusionPathsFromObject(obj);

        Points instances_shifts;
        for (const PrintInstance& inst : obj->instances())
            instances_shifts.emplace_back(inst.shift);

        conflictQueue.emplace_back_bucket(std::move(layers.first), obj, instances_shifts);
        conflictQueue.emplace_back_bucket(std::move(layers.second), obj, instances_shifts);
    }
    conflictQueue.build_queue();

    std::vector<LineWithIDs> layersLines;
    std::vector<double>      heights;
    while (conflictQueue.valid()) {
        LineWithIDs lines     = conflictQueue.getCurLines();
        double      curHeight = conflictQueue.removeLowests();
        heights.push_back(curHeight);
        layersLines.push_back(std::move(lines));
    }

    bool                                   find = false;
    tbb::concurrent_vector<std::tuple<ConflictComputeResult, double, int>> conflict;

    tbb::parallel_for(tbb::blocked_range<size_t>(0, layersLines.size()), [&](tbb::blocked_range<size_t> range) {
        for (size_t i = range.begin(); i < range.end(); i++) {
            auto interRes = find_inter_of_lines(layersLines[i]);
            if (interRes.has_value()) {
                find = true;
                conflict.emplace_back(*interRes, heights[i], i);
                break;
            }
        }
    });

    if (find) {
        std::sort(conflict.begin(), conflict.end(), [](const std::tuple<ConflictComputeResult, double, int>& i1, const std::tuple<ConflictComputeResult, double, int>& i2) {
            return std::get<1>(i1) < std::get<1>(i2);
        });

        auto &[ccr, conflict_height, conflict_layer_id] = conflict[0];
        const void *ptr1           = conflictQueue.idToObjsPtr(ccr._obj1);
        const void *ptr2           = conflictQueue.idToObjsPtr(ccr._obj2);
        if (ptr1 == &wtptr || ptr2 == &wtptr) {
            assert(! wipe_tower_data.z_and_depth_pairs.empty());
            if (ptr2 == &wtptr) { std::swap(ptr1, ptr2); }
            assert(ptr2 != &wtptr);
            if (ptr2 == &wtptr) { return {}; }
            const PrintObject *obj2 = reinterpret_cast<const PrintObject *>(ptr2);
            return std::make_optional<ConflictResult>("WipeTower", obj2->model_object()->name, conflict_height, nullptr, ptr2, conflict_layer_id);
        }
        const PrintObject *obj1 = reinterpret_cast<const PrintObject *>(ptr1);
        const PrintObject *obj2 = reinterpret_cast<const PrintObject *>(ptr2);
        return std::make_optional<ConflictResult>(obj1->model_object()->name, obj2->model_object()->name, conflict_height, ptr1, ptr2, conflict_layer_id);
    } else
        return {};
}

ConflictComputeOpt ConflictChecker::line_intersect(const LineWithID &l1, const LineWithID &l2)
{
    if (l1._obj_id == l2._obj_id && l1._inst_id == l2._inst_id) { return {}; } // lines are from same instance

    Point inter;
    bool  intersect = l1._line.intersection(l2._line, &inter);
    if (intersect) {
        auto dist1 = std::min(unscale_p(Point(l1._line.a - inter)).norm(), unscale_p(Point(l1._line.b - inter)).norm());
        auto dist2 = std::min(unscale_p(Point(l2._line.a - inter)).norm(), unscale_p(Point(l2._line.b - inter)).norm());
        auto dist  = std::min(dist1, dist2);
        if (dist > 0.01) { return std::make_optional<ConflictComputeResult>(l1._obj_id, l2._obj_id); } // the two lines intersects if dist>0.01mm
    }
    return {};
}


} // namespace Slic3r
