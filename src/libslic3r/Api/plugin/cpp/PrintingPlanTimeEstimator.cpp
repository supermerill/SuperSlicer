///|/ Copyright (c) SuperSlicer 2026 Durand Remi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/

/*
PrintingPlan movement-time estimation
=====================================

This source mirrors the movement geometry consumed by G-code generation, but
keeps timing deliberately simple. It resolves inherited speed and Z metadata,
measures each segment at constant speed, and inserts a straight synthetic
travel whenever two consecutive leaves are disconnected.
*/

#include "PrintingPlanTimeEstimator.hpp"

#include <algorithm>
#include <cmath>
#include <optional>
#include <stdexcept>

namespace slic3r_api {
namespace {

struct Position3d
{
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;
};

struct EstimatorState
{
    float speed_mm_per_s = -1.f;
    coord_t z_offset = 0;
};

double planar_segment_length(const c_extrusion_segment &segment);
double distance_3d(const Position3d &lhs, const Position3d &rhs);
Position3d segment_position(c_point point, coord_t print_z, coord_t z_offset);
void estimate_entity(const ExtrusionEntity &entity,
                     const EstimatorState &parent_state,
                     coord_t print_z,
                     double travel_speed,
                     std::optional<Position3d> &position,
                     double &duration);
void estimate_extrusion(const PrintingExtrusion &extrusion,
                        coord_t print_z,
                        double travel_speed,
                        std::optional<Position3d> &position,
                        double &duration);

double planar_segment_length(const c_extrusion_segment &segment)
{
    const double dx = double(segment.point_b.x) - double(segment.point_a.x);
    const double dy = double(segment.point_b.y) - double(segment.point_a.y);
    const double chord = std::hypot(dx, dy);
    if (segment.radius == 0.f)
        return unscaled(chord);

    const double radius = std::abs(double(segment.radius));
    if (radius <= 0.0 || chord > radius * 2.0 + double(SCALED_EPSILON))
        throw std::invalid_argument("PrintingPlan contains an arc with an invalid radius.");

    // Positive radii select the short arc; negative radii select the remaining
    // long arc between the same endpoints.
    const double ratio = std::clamp(chord / (2.0 * radius), 0.0, 1.0);
    double angle = 2.0 * std::asin(ratio);
    if (segment.radius < 0.f)
        angle = 2.0 * PI - angle;
    return unscaled(radius * angle);
}

double distance_3d(const Position3d &lhs, const Position3d &rhs)
{
    return std::sqrt((rhs.x - lhs.x) * (rhs.x - lhs.x) +
                     (rhs.y - lhs.y) * (rhs.y - lhs.y) +
                     (rhs.z - lhs.z) * (rhs.z - lhs.z));
}

Position3d segment_position(c_point point, coord_t print_z, coord_t z_offset)
{
    return Position3d{unscaled(point.x), unscaled(point.y), unscaled(print_z + z_offset)};
}

void estimate_entity(const ExtrusionEntity &entity,
                     const EstimatorState &parent_state,
                     coord_t print_z,
                     double travel_speed,
                     std::optional<Position3d> &position,
                     double &duration)
{
    // Resolve the same inherited process fields that a firmware session sees
    // before descending into children or measuring this node's local path.
    EstimatorState state = parent_state;
    if (const EPropertySpeed *speed = entity.get(EPropertySpeed::key))
        if (speed->speed_mm_per_s > 0.f)
            state.speed_mm_per_s = speed->speed_mm_per_s;
    if (const EPropertyZOffset *z = entity.get(EPropertyZOffset::key))
        state.z_offset = z->get();

    for (uint32_t child_idx = 0; child_idx < entity.child_count(); ++child_idx)
        estimate_entity(entity.child(child_idx), state, print_z, travel_speed, position, duration);

    if (entity.segment_count() == 0)
        return;
    if (state.speed_mm_per_s <= 0.f || !std::isfinite(state.speed_mm_per_s))
        throw std::invalid_argument("A geometric PrintingPlan leaf has no positive effective speed.");

    const c_extrusion_segment first = entity.segment(0);
    const Position3d first_position = segment_position(
        first.point_a, print_z, state.z_offset + first.z_offset_a);

    // The first known point establishes machine position for free. Every later
    // disconnected leaf receives a straight travel at configured travel speed.
    if (position && distance_3d(*position, first_position) > 0.0) {
        if (travel_speed <= 0.0 || !std::isfinite(travel_speed))
            throw std::invalid_argument("Synthetic PrintingPlan travel needs a positive travel_speed.");
        duration += distance_3d(*position, first_position) / travel_speed;
    }
    position = first_position;

    for (uint32_t segment_idx = 0; segment_idx < entity.segment_count(); ++segment_idx) {
        const c_extrusion_segment segment = entity.segment(segment_idx);
        const Position3d destination = segment_position(
            segment.point_b, print_z, state.z_offset + segment.z_offset_b);
        const double planar = planar_segment_length(segment);
        const double dz = destination.z - position->z;
        duration += std::hypot(planar, dz) / double(state.speed_mm_per_s);
        position = destination;
    }
}

void estimate_extrusion(const PrintingExtrusion &extrusion,
                        coord_t print_z,
                        double travel_speed,
                        std::optional<Position3d> &position,
                        double &duration)
{
    estimate_entity(extrusion.root(), EstimatorState{}, print_z, travel_speed, position, duration);
}

} // namespace

PrintingPlanTimeEstimator::PrintingPlanTimeEstimator(const Config &print_config) :
    m_travel_speed_mm_per_s(print_config.float_or_default("travel_speed", 0.0))
{}

std::vector<PrintingLayerTimeEstimate> PrintingPlanTimeEstimator::estimate(
    const PrintingPlan &plan) const
{
    std::vector<PrintingLayerTimeEstimate> estimates;
    std::optional<Position3d> position;

    // Traverse the final serialized order so inter-layer and inter-object
    // travels are attributed exactly where the file writer will encounter them.
    for (uint32_t group_idx = 0; group_idx < plan.group_count(); ++group_idx) {
        const PrintingGroup group = plan.group(group_idx);
        for (uint32_t layer_idx = 0; layer_idx < group.layer_group_count(); ++layer_idx) {
            const PrintingLayerGroup layer = group.layer_group(layer_idx);
            double duration = 0.0;
            for (uint32_t tool_idx = 0; tool_idx < layer.tool_group_count(); ++tool_idx) {
                const PrintingToolGroup tool = layer.tool_group(tool_idx);
                for (uint32_t extrusion_idx = 0; extrusion_idx < tool.extrusion_count(); ++extrusion_idx)
                    estimate_extrusion(tool.extrusion(extrusion_idx), layer.print_z(),
                                       m_travel_speed_mm_per_s, position, duration);
            }
            estimates.push_back(PrintingLayerTimeEstimate{group_idx, layer_idx, duration});
        }
    }
    return estimates;
}

} // namespace slic3r_api
