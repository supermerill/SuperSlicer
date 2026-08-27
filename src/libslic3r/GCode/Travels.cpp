///|/ Copyright (c) SuperSlicer 2026 Durand R?mi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/

#include "Travels.hpp"

#include <boost/functional/hash.hpp>
#include <boost/math/special_functions/pow.hpp>

#include "libslic3r/ExtrusionEntityVisitors.hpp"
#include "libslic3r/GCode.hpp"
#include "libslic3r/Layer.hpp"
#include "libslic3r/PointUtils.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintObject.hpp"
#include "libslic3r/Config/PrintConfig.hpp"

namespace Slic3r::GCode {
    
ObjectOrExtrusionLinef::ObjectOrExtrusionLinef(const Vec2d &a, const Vec2d &b, size_t object_layer_idx, size_t instance_idx, const ExtrusionEntity *ee)
        : Linef(a, b), object_layer_idx(int(object_layer_idx)), instance_idx(int(instance_idx)), extrusion_entity_id(ee->get_id()) {}

bool ExtrudedExtrusionEntity::operator==(const ExtrudedExtrusionEntity &other) const
{
    bool result = extrusion_entity_id == other.extrusion_entity_id &&
        object_layer_idx == other.object_layer_idx && instance_idx == other.instance_idx;
    return result;
}

size_t ExtrudedExtrusionEntityHash::operator()(const ExtrudedExtrusionEntity &eee) const noexcept
{
    std::size_t seed = std::hash<uint64_t>{}(eee.extrusion_entity_id);
    boost::hash_combine(seed, std::hash<int>{}(eee.object_layer_idx));
    boost::hash_combine(seed, std::hash<int>{}(eee.instance_idx));
    return seed;
}

class ExtPeriExtrusionToLines : public ExtrusionVisitorConst {
public:
#ifdef _DEBUG
    std::unordered_set<ExtrudedExtrusionEntity, ExtrudedExtrusionEntityHash> *registered_extrusion;
    std::map<size_t, std::set<uint64_t>> all_ee_id_per_instance;
#endif
    std::vector<ObjectOrExtrusionLinef> lines;
    size_t object_layer_idx;
    size_t instance_idx;
    const PrintInstance *instance;
    const ExtrusionEntity* root_extrusion;
    void default_use(const ExtrusionEntity &entity) override {
        if (!entity.is_leaf()) {
            const ExtrusionEntity *saved_root = root_extrusion;
            if (entity.is_continuous())
                root_extrusion = &entity;
            for (const ExtrusionEntityUPtr &child : entity.children())
                if (child)
                    child->visit(*this);
            root_extrusion = saved_root;
            return;
        }
        if (entity.role().is_external_perimeter()) {
            for (const Line &line : to_lines(entity.as_polyline().to_polyline())) {
                lines.emplace_back(unscale_p(Point{line.a + instance->shift}), unscale_p(Point{line.b + instance->shift}),
                                   object_layer_idx, instance_idx, root_extrusion ? root_extrusion : &entity);
#ifdef _DEBUG
                if (all_ee_id_per_instance[instance_idx].find(root_extrusion ? root_extrusion->get_id() : entity.get_id()) ==
                    all_ee_id_per_instance[instance_idx].end()) {
                    ExtrudedExtrusionEntity eee = {int(object_layer_idx), int(instance_idx), root_extrusion ? root_extrusion->get_id() : entity.get_id()};
                    this->registered_extrusion->insert(eee);
                    all_ee_id_per_instance[instance_idx].insert(root_extrusion ? root_extrusion->get_id() :
                                                                                 entity.get_id());
                }
#endif
            }
        }
    }
    void process(const ExtrusionEntity *root) {
        root_extrusion = nullptr;
        root->visit(*this);
    }
};

AABBTreeLines::LinesDistancer<ObjectOrExtrusionLinef> get_previous_layer_distancer(
    const GCodeGenerator::ObjectsLayerToPrint &objects_to_print, const ExPolygons &slices
) {
    std::vector<ObjectOrExtrusionLinef> lines;
    for (const GCodeGenerator::ObjectLayerToPrint &object_to_print : objects_to_print) {
        if (const PrintObject *object = object_to_print.object(); object) {
            const size_t object_layer_idx = &object_to_print - &objects_to_print.front();
            for (const PrintInstance &instance : object->instances()) {
                const size_t instance_idx = &instance - &object->instances().front();
                for (const ExPolygon &polygon : slices)
                    for (const Line &line : polygon.lines())
                        lines.emplace_back(unscale_p(Point{line.a + instance.shift}), unscale_p(Point{line.b + instance.shift}), object_layer_idx, instance_idx);
            }
        }
    }

    return AABBTreeLines::LinesDistancer{std::move(lines)};
}

std::pair<AABBTreeLines::LinesDistancer<ObjectOrExtrusionLinef>, size_t> TravelObstacleTracker::get_current_layer_distancer(const ObjectsLayerToPrint &objects_to_print)
{
    std::vector<ObjectOrExtrusionLinef> lines;
    size_t extrusion_entity_cnt = 0;
    ExtPeriExtrusionToLines visitor;
#ifdef _DEBUG
    visitor.registered_extrusion = &this->m_registered_extrusion;
#endif
    for (const ObjectLayerToPrint &object_to_print : objects_to_print) {
        visitor.object_layer_idx = &object_to_print - &objects_to_print.front();
        if (const Layer *layer = object_to_print.object_layer; layer) {
            for (const PrintInstance &instance : layer->object()->instances()) {
                visitor.instance = &instance;
                visitor.instance_idx = &instance - &layer->object()->instances().front();
                for (const LayerSliceIsland &layer_island_ptr : layer->islands()) {
                    for (const LayerRegionIsland &region_island_ptr : layer_island_ptr.regions_islands()) {
                        const LayerRegion &layerm = **region_island_ptr.regions().begin(); // FIXME multiple regions
                        if (region_island_ptr.has_extrusion(LayerRegionIsland::PERIMETERS)) {
                            for (const ExtrusionEntity *ee : region_island_ptr.extrusion(LayerRegionIsland::PERIMETERS)) {
                                visitor.process(ee);
                                ++extrusion_entity_cnt;
                            }
                        }
                    }
                }
            }
        }
    }

    return {AABBTreeLines::LinesDistancer{std::move(visitor.lines)}, extrusion_entity_cnt};
}

void TravelObstacleTracker::init_layer(const Layer &layer, const ObjectsLayerToPrint &objects_to_print)
{
    size_t extrusion_entity_cnt = 0;
    m_extruded_extrusion.clear();

    m_objects_to_print         = objects_to_print;
    m_previous_layer_distancer = get_previous_layer_distancer(m_objects_to_print, layer.lower_layer->lslices());

    std::tie(m_current_layer_distancer, extrusion_entity_cnt) = get_current_layer_distancer(m_objects_to_print);
    m_extruded_extrusion.reserve(extrusion_entity_cnt);
}

class InsertExternalPeriExtruded : public ExtrusionVisitorConst {
public:
#ifdef _DEBUG
    std::unordered_set<ExtrudedExtrusionEntity, ExtrudedExtrusionEntityHash> *registered_extrusion;
    TravelObstacleTracker *tracker;
#endif
    std::unordered_set<ExtrudedExtrusionEntity, ExtrudedExtrusionEntityHash> &extruded_extrusion;
    size_t object_layer_idx;
    size_t instance_idx;
    InsertExternalPeriExtruded(std::unordered_set<ExtrudedExtrusionEntity, ExtrudedExtrusionEntityHash> &extr_extr,
                               size_t object_layer_idx,
                               size_t instance_idx)
        : extruded_extrusion(extr_extr), object_layer_idx(object_layer_idx), instance_idx(instance_idx) {}
    void default_use(const ExtrusionEntity &entity) override {
        if (!entity.is_leaf()) {
            if (entity.is_continuous()) {
                if (entity.has_role(ExtrusionRole::ExternalPerimeter)) {
#ifdef _DEBUG
                    assert(registered_extrusion->find({int(object_layer_idx), int(instance_idx), entity.get_id()}) != registered_extrusion->end());
#endif
                    this->extruded_extrusion.insert({int(object_layer_idx), int(instance_idx), entity.get_id()});
                }
                return;
            }
            for (const ExtrusionEntityUPtr &child : entity.children())
                if (child)
                    child->visit(*this);
            return;
        }
        if (entity.role().is_external_perimeter()) {
#ifdef _DEBUG
            assert(registered_extrusion->find({int(object_layer_idx), int(instance_idx), entity.get_id()}) != registered_extrusion->end());
#endif
            this->extruded_extrusion.insert({int(object_layer_idx), int(instance_idx), entity.get_id()});
        }
    }
    void process(const ExtrusionEntity *root) {
        root->visit(*this);
    }
};

void TravelObstacleTracker::mark_extruded(const ExtrusionEntity *extrusion_entity, size_t object_layer_idx, size_t instance_idx)
{
    assert(extrusion_entity);
    InsertExternalPeriExtruded visitor(this->m_extruded_extrusion, object_layer_idx, instance_idx);
#ifdef _DEBUG
    visitor.registered_extrusion = &this->m_registered_extrusion;
    visitor.tracker = this;
#endif
    visitor.process(extrusion_entity);
}

bool TravelObstacleTracker::is_extruded(const ObjectOrExtrusionLinef &line) const
{
    return m_extruded_extrusion.find({line.object_layer_idx, line.instance_idx, line.extrusion_entity_id}) != m_extruded_extrusion.end();
}

} // namespace Slic3r::GCode

namespace Slic3r::GCode::Impl::Travels {

ElevatedTravelFormula::ElevatedTravelFormula(const ElevatedTravelParams &params)
    : smoothing_from(params.slope_end - params.blend_width / 2.0)
    , smoothing_to(params.slope_end + params.blend_width / 2.0)
    , blend_width(params.blend_width)
    , lift_height(params.lift_height)
    , slope_end(params.slope_end) {
    if (smoothing_from < 0) {
        smoothing_from = params.slope_end;
        smoothing_to = params.slope_end;
    }
}

double parabola(const double x, const double a, const double b, const double c) {
    return a * x * x + b * x + c;
}

double ElevatedTravelFormula::slope_function(double distance_from_start) const {
    if (distance_from_start < this->slope_end) {
        const double lift_percent = distance_from_start / this->slope_end;
        return lift_percent * this->lift_height;
    } else {
        return this->lift_height;
    }
}

double ElevatedTravelFormula::operator()(const double distance_from_start) const {
    if (distance_from_start > this->smoothing_from && distance_from_start < this->smoothing_to) {
        const double slope = this->lift_height / this->slope_end;

        // This is a part of a parabola going over a specific
        // range and with specific end slopes.
        const double a = -slope / 2.0 / this->blend_width;
        const double b = slope * this->smoothing_to / this->blend_width;
        const double c = this->lift_height + a * boost::math::pow<2>(this->smoothing_to);
        return parabola(distance_from_start, a, b, c);
    }
    return slope_function(distance_from_start);
}

Points3 generate_flat_travel(tcb::span<const Point> xy_path, const float elevation) {
    Points3 result;
    result.reserve(xy_path.size());
    for (const Point &point : xy_path) {
        result.emplace_back(point.x(), point.y(), scale_i(elevation));
    }
    return result;
}

std::vector<DistancedPoint> slice_xy_path(tcb::span<const Point> xy_path,
                                          tcb::span<const double> sorted_distances,
                                          coordf_t min_distance) {
    assert(xy_path.size() >= 2);
    std::vector<DistancedPoint> result;
    result.reserve(xy_path.size() + sorted_distances.size());
    coordf_t total_distance{0};
    result.emplace_back(DistancedPoint{xy_path.front(), 0});
    std::size_t dist_idx{0};
    for (const Point &point : xy_path.subspan(1)) {
        const double current_segment_end = total_distance + point.distance_to(result.back().point);
        for (const double distance_to_add : sorted_distances.subspan(dist_idx)) {
            coordf_t dist_target = scale_d(distance_to_add);
            if (dist_target < current_segment_end + min_distance) {
                if (dist_target + min_distance > current_segment_end) {
                    total_distance = current_segment_end;
                    result.emplace_back(DistancedPoint{point, total_distance});
                } else {
                    //don't bother if it's at less than epsilon from the last point
                    if (dist_target > total_distance + min_distance) {
                        result.emplace_back(
                            DistancedPoint{Line(result.back().point, point).point_at(dist_target - total_distance), dist_target});
                        total_distance = dist_target;
                    }
                }
                ++dist_idx;
            } else {
                break;
            }
        }
        if (total_distance < current_segment_end) {
            assert(point.distance_to(result.back().point) > SCALED_EPSILON);
            total_distance = current_segment_end;
            result.emplace_back(DistancedPoint{point, total_distance});
        } else {
            assert(result.back().point == point);
        }
    }
    assert(result.back().point == xy_path.back());
    return result;
}

Points3 generate_elevated_travel(
    const tcb::span<const Point> xy_path,
    const std::vector<double> &ensure_points_at_distances,
    const double initial_elevation,
    const std::function<double(double)> &elevation
) {
    Points3 result{};

    std::vector<DistancedPoint> extended_xy_path = slice_xy_path(xy_path, ensure_points_at_distances);
    result.reserve(extended_xy_path.size());

    for (const DistancedPoint &point : extended_xy_path) {
        result.emplace_back(
            point.point.x(), point.point.y(),
            scale_i(initial_elevation + elevation(unscaled(point.dist_from_start)))
        );
    }

    return result;
}

struct Intersection
{
    int  object_layer_idx = -1;
    int  instance_idx     = -1;
    bool is_inside        = false;

    bool is_print_instance_equal(const ObjectOrExtrusionLinef &print_istance) {
        return this->object_layer_idx == print_istance.object_layer_idx && this->instance_idx == print_istance.instance_idx;
    }
};

double get_first_crossed_line_distance(
    tcb::span<const Line> xy_path,
    const AABBTreeLines::LinesDistancer<ObjectOrExtrusionLinef> &distancer,
    const ObjectsLayerToPrint &objects_to_print,
    const std::function<bool(const ObjectOrExtrusionLinef &)> &predicate,
    const bool ignore_starting_object_intersection
) {
    assert(!xy_path.empty());
    if (xy_path.empty())
        return std::numeric_limits<double>::max();

    const Point path_first_point = xy_path.front().a;
    double traversed_distance = 0;
    bool skip_intersection = ignore_starting_object_intersection;
    Intersection first_intersection;

    for (const Line &line : xy_path) {
        const ObjectOrExtrusionLinef                    unscaled_line = {unscale_p(line.a), unscale_p(line.b)};
        const std::vector<std::pair<Vec2d, size_t>>     intersections = distancer.intersections_with_line<true>(unscaled_line);

        if (intersections.empty())
            continue;

        if (!objects_to_print.empty() && ignore_starting_object_intersection && first_intersection.object_layer_idx == -1) {
            const ObjectOrExtrusionLinef &intersection_line = distancer.get_line(intersections.front().second);
            const Point shift = objects_to_print[intersection_line.object_layer_idx].layer()->object()->instances()[intersection_line.instance_idx].shift;
            const Point shifted_first_point = path_first_point - shift;
            const bool contain_first_point = expolygons_contain(objects_to_print[intersection_line.object_layer_idx].layer()->lslices(), shifted_first_point);

            first_intersection = {intersection_line.object_layer_idx, intersection_line.instance_idx, contain_first_point};
        }

        for (const auto &intersection : intersections) {
            const ObjectOrExtrusionLinef &intersection_line = distancer.get_line(intersection.second);
            const double distance = traversed_distance + (unscaled_line.a - intersection.first).norm();
            if (distance <= EPSILON)
                continue;

            // There is only one external border for each object, so when we cross this border,
            // we definitely know that we are outside the object.
            if (skip_intersection && first_intersection.is_print_instance_equal(intersection_line) && first_intersection.is_inside) {
                skip_intersection = false;
                continue;
            }

            if (!predicate(intersection_line))
                continue;

            return distance;
        }

        traversed_distance += (unscaled_line.a - unscaled_line.b).norm();
    }

    return std::numeric_limits<double>::max();
}

double get_obstacle_adjusted_slope_end(const Lines &xy_path, const GCode::TravelObstacleTracker &obstacle_tracker) {
    const double previous_layer_crossed_line = get_first_crossed_line_distance(
        xy_path, obstacle_tracker.previous_layer_distancer(), obstacle_tracker.objects_to_print()
    );
    const double current_layer_crossed_line = get_first_crossed_line_distance(
        xy_path, obstacle_tracker.current_layer_distancer(), obstacle_tracker.objects_to_print(),
        [&obstacle_tracker](const ObjectOrExtrusionLinef &line) { return obstacle_tracker.is_extruded(line); }
    );

    return std::min(previous_layer_crossed_line, current_layer_crossed_line);
}

struct SmoothingParams
{
    double blend_width{};
    unsigned points_count{1};
};

SmoothingParams get_smoothing_params(
    const double lift_height,
    const double slope_end,
    unsigned extruder_id,
    const double travel_length,
    const FullPrintConfig &config
) {
    if (config.gcode_flavor != gcfMarlinFirmware)
        // Smoothing is supported only on Marlin.
        return {0, 1};

    const double slope = lift_height / slope_end;
    const double max_machine_z_velocity = config.machine_max_feedrate_z.get_at(extruder_id);
    const double max_xy_velocity =
        Vec2d{
            config.machine_max_feedrate_x.get_at(extruder_id),
            config.machine_max_feedrate_y.get_at(extruder_id)}
            .norm();

    const double xy_acceleration = config.machine_max_acceleration_travel.get_at(extruder_id);

    const double xy_acceleration_time = max_xy_velocity / xy_acceleration;
    const double xy_acceleration_distance = 1.0 / 2.0 * xy_acceleration *
        boost::math::pow<2>(xy_acceleration_time);

    if (travel_length < xy_acceleration_distance) {
        return {0, 1};
    }

    const double max_z_velocity = std::min(max_xy_velocity * slope, max_machine_z_velocity);
    const double deceleration_time = max_z_velocity /
        config.machine_max_acceleration_z.get_at(extruder_id);
    const double deceleration_xy_distance = deceleration_time * max_xy_velocity;

    double blend_width = slope_end > deceleration_xy_distance / 2.0 ? deceleration_xy_distance :
                                                                          slope_end * 2.0;

    // not possible to blend into not-existing travel
    if (travel_length < slope_end + blend_width / 2.0) {
        blend_width = (travel_length - slope_end) * 2;
    }

    const unsigned points_count = blend_width > 0 ?
        std::ceil(max_z_velocity / config.machine_max_jerk_z.get_at(extruder_id)) :
        1;

    if (blend_width <= 0     // When there is no blend with, there is no need for smoothing.
        || points_count > 6  // That would be way to many points. Do not do it at all.
        || points_count <= 0 // Always return at least one point.
    )
        return {0, 1};

    return {blend_width, points_count};
}

ElevatedTravelParams get_elevated_traval_params(
    const Polyline& xy_path,
    const FullPrintConfig &config,
    GCodeWriter writer,
    const GCode::TravelObstacleTracker &obstacle_tracker,
    size_t layer_id,
    double desired_z_lift
) {
    ElevatedTravelParams elevation_params{};
    assert(writer.tool());
    uint16_t extruder_id  = writer.tool()->id();
    if (!config.travel_ramping_lift.get_at(extruder_id)) {
        elevation_params.slope_end = 0;
        writer.lift(layer_id);
        elevation_params.lift_height = writer.get_position().z();
        writer.unlift();
        elevation_params.lift_height -= writer.get_position().z();
        elevation_params.blend_width = 0;
        return elevation_params;
    }
    elevation_params.lift_height = desired_z_lift;

    const double slope_deg = config.travel_slope.get_at(extruder_id);

    if (slope_deg >= 90 || slope_deg <= 0) {
        elevation_params.slope_end = 0;
    } else {
        const double slope_rad = slope_deg * (M_PI / 180); // rad
        elevation_params.slope_end = elevation_params.lift_height / std::tan(slope_rad);
    }

    const double obstacle_adjusted_slope_end = get_obstacle_adjusted_slope_end(xy_path.lines(), obstacle_tracker);
    if (obstacle_adjusted_slope_end < elevation_params.slope_end)
        elevation_params.slope_end = obstacle_adjusted_slope_end;

    SmoothingParams smoothing_params{get_smoothing_params(
        elevation_params.lift_height, elevation_params.slope_end, extruder_id,
        unscaled(xy_path.length()), config
    )};

    elevation_params.blend_width = smoothing_params.blend_width;
    elevation_params.parabola_points_count = smoothing_params.points_count;
    return elevation_params;
}

std::vector<double> linspace(const double from, const double to, const unsigned count) {
    if (count == 0) {
        return {};
    }
    std::vector<double> result;
    result.reserve(count);
    if (count == 1) {
        result.emplace_back((from + to) / 2.0);
        return result;
    }
    const double step = (to - from) / count;
    for (unsigned i = 0; i < count - 1; ++i) {
        result.emplace_back(from + i * step);
    }
    result.emplace_back(to); // Make sure the last value is exactly equal to the value of "to".
    return result;
}
} // namespace Slic3r::GCode::Impl::Travels
