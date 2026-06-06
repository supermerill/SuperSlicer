///|/ Copyright (c) SuperSlicer 2026 Durand R?mi @supermerill
///|/ Copyright (c) Prusa Research 2017 - 2022 Vojtěch Bubník @bubnikv, Lukáš Matěna @lukasmatena
///|/ Copyright (c) 2019 Thomas Moore
///|/
///|/ PrusaSlicer is released under the terms of the AGPLv3 or higher
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/

#include "PrintExtents.hpp"

#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/ExtrusionEntity.hpp"
#include "libslic3r/ExtrusionEntityCollection.hpp"
#include "libslic3r/Layer.hpp"
#include "libslic3r/PointUtils.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintObject.hpp"

#include "WipeTower.hpp"

// Calculate extents of the extrusions assigned to Print / PrintObject.
// The extents are used for assessing collisions of the print with the priming towers,
// to decide whether to pause the print after the priming towers are extruded
// to let the operator remove them from the print bed.
namespace Slic3r {

static inline BoundingBox extrusion_polyline_extents(const Polyline &polyline, const coord_t radius)
{
    BoundingBox bbox;
    if (! polyline.points.empty())
        bbox.merge(polyline.points.front());
    for (const Point &pt : polyline.points) {
        bbox.min(0) = std::min(bbox.min(0), pt(0) - radius);
        bbox.min(1) = std::min(bbox.min(1), pt(1) - radius);
        bbox.max(0) = std::max(bbox.max(0), pt(0) + radius);
        bbox.max(1) = std::max(bbox.max(1), pt(1) + radius);
    }
    return bbox;
}

static inline BoundingBoxf extrusionentity_extents(const ExtrusionPath &extrusion_path)
{
    BoundingBox bbox = extrusion_polyline_extents(extrusion_path.polyline().to_polyline(), scale_i(0.5 * extrusion_path.width()));
    BoundingBoxf bboxf;
    if (! empty(bbox)) {
        bboxf.min = unscale_p(bbox.min);
        bboxf.max = unscale_p(bbox.max);
		bboxf.defined = true;
    }
    return bboxf;
}

static inline BoundingBoxf extrusionentity_extents(const ExtrusionLoop &extrusion_loop)
{
    BoundingBox bbox;
    for (const ExtrusionPath &extrusion_path : extrusion_loop.paths())
        bbox.merge(extrusion_polyline_extents(extrusion_path.polyline().to_polyline(), scale_i(0.5 * extrusion_path.width())));
    BoundingBoxf bboxf;
    if (! empty(bbox)) {
        bboxf.min = unscale_p(bbox.min);
        bboxf.max = unscale_p(bbox.max);
		bboxf.defined = true;
	}
    return bboxf;
}

static inline BoundingBoxf extrusionentity_extents(const ExtrusionMultiPath &extrusion_multi_path)
{
    BoundingBox bbox;
    for (const ExtrusionPath &extrusion_path : extrusion_multi_path.paths())
        bbox.merge(extrusion_polyline_extents(extrusion_path.polyline().to_polyline(), scale_i(0.5 * extrusion_path.width())));
    BoundingBoxf bboxf;
    if (! empty(bbox)) {
        bboxf.min = unscale_p(bbox.min);
        bboxf.max = unscale_p(bbox.max);
		bboxf.defined = true;
	}
    return bboxf;
}

static BoundingBoxf extrusionentity_extents(const ExtrusionEntity *extrusion_entity);

static inline BoundingBoxf extrusionentity_extents(const ExtrusionEntityCollection &extrusion_entity_collection)
{
    BoundingBoxf bbox;
    for (const ExtrusionEntity *extrusion_entity : extrusion_entity_collection.entities())
        bbox.merge(extrusionentity_extents(extrusion_entity));
    return bbox;
}

static BoundingBoxf extrusionentity_extents(const ExtrusionEntity *extrusion_entity)
{
    if (extrusion_entity == nullptr)
        return BoundingBoxf();
    auto *extrusion_path = dynamic_cast<const ExtrusionPath*>(extrusion_entity);
    if (extrusion_path != nullptr)
        return extrusionentity_extents(*extrusion_path);
    auto *extrusion_loop = dynamic_cast<const ExtrusionLoop*>(extrusion_entity);
    if (extrusion_loop != nullptr)
        return extrusionentity_extents(*extrusion_loop);
    auto *extrusion_multi_path = dynamic_cast<const ExtrusionMultiPath*>(extrusion_entity);
    if (extrusion_multi_path != nullptr)
        return extrusionentity_extents(*extrusion_multi_path);
    auto *extrusion_entity_collection = dynamic_cast<const ExtrusionEntityCollection*>(extrusion_entity);
    if (extrusion_entity_collection != nullptr)
        return extrusionentity_extents(*extrusion_entity_collection);
    throw Slic3r::RuntimeError("Unexpected extrusion_entity type in extrusionentity_extents()");
    return BoundingBoxf();
}

BoundingBoxf get_print_extrusions_extents(const Print &print)
{
    BoundingBoxf bbox(extrusionentity_extents(print.brim()));
    bbox.merge(extrusionentity_extents(print.skirt()));
    return bbox;
}

BoundingBoxf get_print_object_extrusions_extents(const PrintObject &print_object, const coord_t max_print_z)
{
    BoundingBoxf bbox;
    for (const Layer &layer : print_object.layers()) {
        if (layer.scaled_print_z() > max_print_z)
            break;
        BoundingBoxf bbox_this;
        for (const LayerSliceIsland &layer_island_ptr : layer.islands()) {
            for (const LayerRegionIsland &region_island_ptr : layer_island_ptr.regions_islands()) {
                if (region_island_ptr.has_extrusion(LayerRegionIsland::PERIMETERS)) {
                    bbox_this.merge(extrusionentity_extents(region_island_ptr.extrusion(LayerRegionIsland::PERIMETERS)));
                }
                if (region_island_ptr.has_extrusion(LayerRegionIsland::INFILLS)) {
                    bbox_this.merge(extrusionentity_extents(region_island_ptr.extrusion(LayerRegionIsland::INFILLS)));
                }
                // 2 next take care of the case 'layer.get_property<LayerSupportProperty>() != nullptr;'
                if (region_island_ptr.has_extrusion(LayerRegionIsland::SUPPORT)) {
                    bbox_this.merge(extrusionentity_extents(region_island_ptr.extrusion(LayerRegionIsland::SUPPORT)));
                }
                if (region_island_ptr.has_extrusion(LayerRegionIsland::SUPPORT_INTERFACE)) {
                    bbox_this.merge(extrusionentity_extents(region_island_ptr.extrusion(LayerRegionIsland::SUPPORT_INTERFACE)));
                }
            }
        }
        for (const PrintInstance &instance : print_object.instances()) {
            BoundingBoxf bbox_translated(bbox_this);
            bbox_translated.translate(unscale_p(instance.shift));
            bbox.merge(bbox_translated);
        }
    }
    return bbox;
}

// Returns a bounding box of a projection of the wipe tower for the layers <= max_print_z.
// The projection does not contain the priming regions.
BoundingBoxf get_wipe_tower_extrusions_extents(const Print &print, const coord_t max_print_z)
{
    // Wipe tower extrusions are saved as if the tower was at the origin with no rotation
    // We need to get position and angle of the wipe tower to transform them to actual position.
    Transform2d trafo =
        Eigen::Translation2d(print.default_object_config().wipe_tower_x.value, print.default_object_config().wipe_tower_y.value) *
        Eigen::Rotation2Dd(Geometry::deg2rad(print.default_object_config().wipe_tower_rotation_angle.value));

    BoundingBoxf bbox;
    for (const std::vector<WipeTower::ToolChangeResult> &tool_changes : print.wipe_tower_data().tool_changes) {
        if (! tool_changes.empty() && scale_to_layer_coord(tool_changes.front().print_z) > max_print_z)
            break;
        for (const WipeTower::ToolChangeResult &tcr : tool_changes) {
            for (size_t i = 1; i < tcr.extrusions.size(); ++ i) {
                const WipeTower::Extrusion &e = tcr.extrusions[i];
                if (e.width > 0) {
                    Vec2d delta = 0.5 * Vec2d(e.width, e.width);
                    Vec2d p1 = trafo * (&e - 1)->pos.cast<double>();
                    Vec2d p2 = trafo * e.pos.cast<double>();
                    bbox.merge(p1.cwiseMin(p2) - delta);
                    bbox.merge(p1.cwiseMax(p2) + delta);
                }
            }
        }
    }
    return bbox;
}

// Returns a bounding box of the wipe tower priming extrusions.
BoundingBoxf get_wipe_tower_priming_extrusions_extents(const Print &print)
{
    BoundingBoxf bbox;
    if (print.wipe_tower_data().priming != nullptr) {
        for (const WipeTower::ToolChangeResult &tcr : *print.wipe_tower_data().priming) {
            for (size_t i = 1; i < tcr.extrusions.size(); ++ i) {
                const WipeTower::Extrusion &e = tcr.extrusions[i];
                if (e.width > 0) {
                    const Vec2d& p1 = (&e - 1)->pos.cast<double>();
                    const Vec2d& p2 = e.pos.cast<double>();
                    bbox.merge(p1);
                    coordf_t radius = 0.5 * e.width;
                    bbox.min(0) = std::min(bbox.min(0), std::min(p1(0), p2(0)) - radius);
                    bbox.min(1) = std::min(bbox.min(1), std::min(p1(1), p2(1)) - radius);
                    bbox.max(0) = std::max(bbox.max(0), std::max(p1(0), p2(0)) + radius);
                    bbox.max(1) = std::max(bbox.max(1), std::max(p1(1), p2(1)) + radius);
                }
            }
        }
    }
    return bbox;
}

}
