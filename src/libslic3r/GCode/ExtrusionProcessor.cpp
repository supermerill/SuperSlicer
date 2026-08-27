///|/ Copyright (c) SuperSlicer 2026 Durand R?mi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/

#include "ExtrusionProcessor.hpp"

#include <string>

#include "libslic3r/PointUtils.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/Config/PrintConfig.hpp"
#include "libslic3r/PrintRegion.hpp"

namespace Slic3r { namespace ExtrusionProcessor {

//
//class CalculateAndSliptOverhangingExtrusionsVisitor : public ExtrusionVisitorConst {
//public:


ExtrusionPaths calculate_and_split_overhanging_extrusions(const ExtrusionPath                             &path,
                                                          const AABBTreeLines::LinesDistancer<Linef>      &unscaled_prev_layer,
                                                          const AABBTreeLines::LinesDistancer<CurledLine> &prev_layer_curled_lines,
                                                          const double &nozzle_diameter) {
    assert(path.overhang_attributes() == nullptr || path.overhang_attributes()->has_full_overhangs_speed ||
           path.overhang_attributes()->has_dynamic_overhangs_speed);
    if (path.overhang_attributes() == nullptr) {
        return { path };
    } else {
        if (!path.overhang_attributes()->has_dynamic_overhangs_flow && !path.overhang_attributes()->has_dynamic_overhangs_speed) {
            // not inside the dynamic range
            //path.overhang_attributes()->start_distance_from_prev_layer = 1;
            //path.overhang_attributes()->end_distance_from_prev_layer = 1;
            return { path };
        }
    }
    //TODO: 'split' lines if the dist of each point is between 0 and max_width, with a max length of path.width/2

    std::vector<ExtendedPoint>           extended_points = estimate_points_properties<true, true, true, true>(path.polyline().to_polyline().points,
                                                                                                    unscaled_prev_layer, path.width(), (float)nozzle_diameter);
    std::vector<std::pair<float, float>> calculated_distances(extended_points.size());

    for (size_t i = 0; i < extended_points.size(); i++) {
        // the path can have some little innacuracie, so we need to make sure it's positive.
        extended_points[i].distance = std::max(0.f, extended_points[i].distance);

        const ExtendedPoint &curr = extended_points[i];
        const ExtendedPoint &next = extended_points[i + 1 < extended_points.size() ? i + 1 : i];

        // The following code artifically increases the distance to provide slowdown for extrusions that are over curled lines
        float        proximity_to_curled_lines = 0.0;
        const double dist_limit                = 10.0 * path.width();
        {
            Vec2d middle       = 0.5 * (curr.position + next.position);
            auto  line_indices = prev_layer_curled_lines.all_lines_in_radius(Point::new_scale(middle), scale_i(dist_limit));
            if (!line_indices.empty()) {
                double len = (next.position - curr.position).norm();
                // For long lines, there is a problem with the additional slowdown. If by accident, there is small curled line near the middle
                // of this long line
                //  The whole segment gets slower unnecesarily. For these long lines, we do additional check whether it is worth slowing down.
                // NOTE that this is still quite rough approximation, e.g. we are still checking lines only near the middle point
                // TODO maybe split the lines into smaller segments before running this alg? but can be demanding, and GCode will be huge
                if (len > 8) {
                    Vec2d dir   = Vec2d(next.position - curr.position) / len;
                    Vec2d right = Vec2d(-dir.y(), dir.x());

                    Polygon box_of_influence = {
                        scale_p(Vec2d(curr.position + right * dist_limit)),
                        scale_p(Vec2d(next.position + right * dist_limit)),
                        scale_p(Vec2d(next.position - right * dist_limit)),
                        scale_p(Vec2d(curr.position - right * dist_limit)),
                    };

                    double projected_lengths_sum = 0;
                    for (size_t idx : line_indices) {
                        const CurledLine &line   = prev_layer_curled_lines.get_line(idx);
                        Lines             inside = intersection_ln({{line.a, line.b}}, {box_of_influence});
                        if (inside.empty())
                            continue;
                        double projected_length = abs(dir.dot(unscale_p(Vec2d((inside.back().b - inside.back().a).cast<double>()))));
                        projected_lengths_sum += projected_length;
                    }
                    if (projected_lengths_sum < 0.4 * len) {
                        line_indices.clear();
                    }
                }

                for (size_t idx : line_indices) {
                    const CurledLine &line                 = prev_layer_curled_lines.get_line(idx);
                    float             distance_from_curled = unscaled(std::sqrt(line_alg::distance_to_squared(line, Point::new_scale(middle))));
                    float proximity = (1.0 - (distance_from_curled / dist_limit)) * (1.0 - (distance_from_curled / dist_limit)) *
                                      (line.curled_height / (path.height() * 10.0f)); // max_curled_height_factor from SupportSpotGenerator
                    proximity_to_curled_lines = std::max(proximity_to_curled_lines, proximity);
                }
            }
        }
        calculated_distances[i].first  = std::max(curr.distance, next.distance);
        calculated_distances[i].second = proximity_to_curled_lines;
    }

    ExtrusionPaths      result;
    result.emplace_back(path.attributes(), path.clone_properties(), path.can_reverse());
    ExtrusionPropertyOverhang overhang_attributes(calculated_distances[0].first,
                                                  calculated_distances[0].first,
                                                  calculated_distances[0].second);
    overhang_attributes.has_full_overhangs_flow = path.overhang_attributes()->has_full_overhangs_flow;
    overhang_attributes.has_full_overhangs_speed = path.overhang_attributes()->has_full_overhangs_speed;
    overhang_attributes.has_dynamic_overhangs_flow = path.overhang_attributes()->has_dynamic_overhangs_flow;
    overhang_attributes.has_dynamic_overhangs_speed = path.overhang_attributes()->has_dynamic_overhangs_speed;
    result.back().overhang_attributes_mutable() = overhang_attributes;
    result.back().polyline().append(Point::new_scale(extended_points[0].position));
    size_t sequence_start_index = 0;
    for (size_t i = 1; i < extended_points.size(); i++) {
        if (!result.back().polyline().back().coincides_with_epsilon(Point::new_scale(extended_points[i].position))) {
            result.back().polyline().append(Point::new_scale(extended_points[i].position));
            result.back().overhang_attributes_mutable().end_distance_from_prev_layer =  extended_points[i].distance;
        }
        if (std::abs(calculated_distances[sequence_start_index].first - calculated_distances[i].first) < 0.01 * nozzle_diameter &&
            std::abs(calculated_distances[sequence_start_index].second - calculated_distances[i].second) < 0.001) {
            // do not start new path, the attributes are similar enough
            // NOTE: a larger tolerance may be applied here. However, it makes the gcode preview much less smooth
            // (But it has very likely zero impact on the print quality.)
        } else if (i + 1 < extended_points.size()) { // do not start new path if this is last point!
            // start new path, parameters differ
            // store the distance in mm
            overhang_attributes.start_distance_from_prev_layer = calculated_distances[i].first;
            overhang_attributes.end_distance_from_prev_layer   = calculated_distances[i].first;
            // already between 0 and 1
            overhang_attributes.proximity_to_curled_lines      = calculated_distances[i].second;
            sequence_start_index                                          = i;
            if (result.back().size() > 1) {
                result.emplace_back(path.attributes(), path.clone_properties(), path.can_reverse());
                result.back().overhang_attributes_mutable() = overhang_attributes;
                result.back().polyline().append(Point::new_scale(extended_points[i].position));
            } else {
                assert(result.back().size() == 1);
                assert(result.back().polyline().back().coincides_with_epsilon(Point::new_scale(extended_points[i].position)));
            }
        }
    }
    // started new path, but there is noting after that.
    if (result.back().size() == 1) {
        //delete it
        result.pop_back();
    }
    //remove overhang role, as it prevents placing seams on it.
    for (ExtrusionPath &res_path : result) {
        assert(res_path.overhang_attributes());
        res_path.attributes_mutable().set_role(res_path.role() & ExtrusionRoleModifier(~ExtrusionRoleModifier::ERM_Bridge));
        //assert(res_path.role() == ExtrusionRole::Perimeter || res_path.role() == ExtrusionRole::ExternalPerimeter);
    }
#ifdef _DEBUG
    for (auto &path : result) {
        assert(path.overhang_attributes() && path.overhang_attributes()->start_distance_from_prev_layer >= 0 &&
               path.overhang_attributes()->end_distance_from_prev_layer >= 0);
    }
    assert(is_approx(result.front().first_point(), path.first_point()));
    assert(is_approx(result.back().last_point(), path.last_point()));
    Point last_pt = result.front().last_point();
    for (size_t idx_path = 1; idx_path < result.size() ; ++idx_path) {
        ExtrusionPath &path = result[idx_path];
        assert(path.polyline().size() >= 2);
        assert(path.first_point().coincides_with_epsilon(last_pt));
        if (!path.polyline().get_point(1).coincides_with_epsilon(last_pt)) {
            path.polyline().set_front(last_pt);
        }
        for (size_t idx_pt = 1; idx_pt < path.size(); ++idx_pt)
            assert(!path.polyline().get_point(idx_pt - 1).coincides_with_epsilon(path.polyline().get_point(idx_pt)));
        last_pt = path.last_point();
    }
#endif
    // avoid precision loss
    result.front().polyline().set_front(path.first_point());
    result.back().polyline().set_back(path.last_point());
    for (const ExtrusionPath &res_path : result) {
        assert(res_path.overhang_attributes() == nullptr ||
               res_path.overhang_attributes()->has_full_overhangs_speed ||
               res_path.overhang_attributes()->has_dynamic_overhangs_speed);
    }
    return result;
};

//class CalculateSplitOverhangVisitor : public ExtrusionVisitorRecursiveConst {
//public:
//    ExtrusionEntities current_collection;
//    const AABBTreeLines::LinesDistancer<Linef> &unscaled_prev_layer;
//    const AABBTreeLines::LinesDistancer<CurledLine> &prev_layer_curled_lines;
//    const double max_width
//    using ExtrusionVisitorRecursiveConst::use;
//    CalculateSplitOverhangVisitor() {
//    }
//    void use(const ExtrusionPath &path) override { volume += unscaled(path3D.length()) * path3D.mm3_per_mm(); }
//    void use(const ExtrusionPath3D &path3D) override { 
//            result.append(std::move(new_mp)); //TODO split
//    }
//    double get(const ExtrusionEntityCollection &coll);
//};

ExtrusionEntityCollection calculate_and_split_overhanging_extrusions(const ExtrusionEntityCollection            *ecc,
                                                                     const AABBTreeLines::LinesDistancer<Linef> &unscaled_prev_layer,
                                                                     const AABBTreeLines::LinesDistancer<CurledLine> &prev_layer_curled_lines,
                                                                     const double &nozzle_diameter)
{
    ExtrusionEntityCollection result{};
    result.set_can_sort_reverse(ecc->can_sort(), ecc->can_reverse());
    for (const auto *e : ecc->entities()) {
        if (auto *col = dynamic_cast<const ExtrusionEntityCollection *>(e)) {
            result.append(calculate_and_split_overhanging_extrusions(col, unscaled_prev_layer, prev_layer_curled_lines, nozzle_diameter));
        } else if (auto *loop = dynamic_cast<const ExtrusionLoop *>(e)) {
#ifdef _DEBUG
    Point last_pt = loop->last_point();
    for (const ExtrusionPath &path : loop->paths()) {
        assert(path.polyline().size() >= 2);
        assert(path.first_point() == last_pt);
        for (size_t idx = 1; idx < path.size(); ++idx)
            assert(!path.polyline().get_point(idx - 1).coincides_with_epsilon(path.polyline().get_point(idx)));
        last_pt = path.last_point();
    }
#endif
            ExtrusionLoop new_loop = *loop;
            new_loop.paths().clear();
            for (const ExtrusionPath &p : loop->paths()) {
                auto paths = calculate_and_split_overhanging_extrusions(p, unscaled_prev_layer, prev_layer_curled_lines, nozzle_diameter);
                assert(p.first_point() == paths.front().first_point());
                assert(p.last_point() == paths.back().last_point());
                new_loop.paths().insert(new_loop.paths().end(), paths.begin(), paths.end());
            }
            result.append(std::move(new_loop));
        } else if (auto *mp = dynamic_cast<const ExtrusionMultiPath *>(e)) {
            ExtrusionMultiPath new_mp = *mp;
            new_mp.paths().clear();
            for (const ExtrusionPath &p : mp->paths()) {
                auto paths = calculate_and_split_overhanging_extrusions(p, unscaled_prev_layer, prev_layer_curled_lines, nozzle_diameter);
                assert(p.first_point() == paths.front().first_point());
                assert(p.last_point() == paths.back().last_point());
                new_mp.paths().insert(new_mp.paths().end(), paths.begin(), paths.end());
            }
            result.append(std::move(new_mp));
        } else if (auto *p = dynamic_cast<const ExtrusionPath *>(e)) {
            // bypass splitting if the extrusion is in 3D 
            if (!p->polyline().has_z_offset()) {
                    result.append(calculate_and_split_overhanging_extrusions(*p, unscaled_prev_layer, prev_layer_curled_lines, nozzle_diameter));
            } else {
                ExtrusionPath new_p = *p;
                result.append(std::move(new_p)); //TODO split
            }
        } else {
            throw Slic3r::InvalidArgument("Unknown extrusion entity type");
        }
    }
    return result;
};


std::pair<float,float> calculate_overhang_speed(const ExtrusionPath &path,
                              const FullPrintConfig     &config,
                              size_t                     extruder_id)
{
    const ExtrusionPropertyOverhang *overhang_attributes = path.overhang_attributes();
    assert(overhang_attributes);
    if(!overhang_attributes)
        return {-1, -1};
    float speed_ratio = 0; // 0: overhangs speed, 1= perimeter/externalperimeter speed.
    float fan_speed = -1;
    if (config.overhangs_dynamic_speed.is_enabled()) {
        if (overhang_attributes->start_distance_from_prev_layer == 0 &&
            overhang_attributes->end_distance_from_prev_layer == 0) {
            speed_ratio = 1;
        } else {
            assert(config.overhangs);
            float max_dynamic_distance =
                (float) (config.overhangs_width_speed.is_enabled() ?
                             config.overhangs_width_speed.get_effective_value(config.nozzle_diameter.get_at(extruder_id)) :
                             config.overhangs_width.get_effective_value(config.nozzle_diameter.get_at(extruder_id)));
            GraphData graph = config.overhangs_dynamic_speed.value;
            // ensure it start at 0%, and ensure it ends at 100%
            if (graph.graph_points[graph.begin_idx].x() != 0) {
                graph.graph_points.insert(graph.graph_points.begin() + graph.begin_idx, {0, 0});
                graph.end_idx++;
            }
            if (graph.graph_points[graph.end_idx - 1].x() != 100) {
                graph.graph_points.insert(graph.graph_points.begin() + graph.end_idx, {100, 100});
                graph.end_idx++;
            }
            graph.graph_points[graph.begin_idx].x() = 0;
            graph.graph_points[graph.end_idx - 1].y() = 100;
            // interpolate
            assert(overhang_attributes->start_distance_from_prev_layer >= 0);
            assert(overhang_attributes->end_distance_from_prev_layer >= 0);
            float extrusion_ratio   = std::min(
                         graph.interpolate(100 - 100 * std::min(1.f, overhang_attributes->start_distance_from_prev_layer / max_dynamic_distance)),
                         graph.interpolate(100 - 100 * std::min(1.f, overhang_attributes->end_distance_from_prev_layer / max_dynamic_distance)));
            assert(path.width() * overhang_attributes->proximity_to_curled_lines >= 0 &&
                   path.width() * overhang_attributes->proximity_to_curled_lines <= 1);
            float curled_extrusion_ratio = graph.interpolate(100 - 100 * overhang_attributes->proximity_to_curled_lines);
            speed_ratio       = std::min(extrusion_ratio, curled_extrusion_ratio) / 100.0;
            assert(speed_ratio >= 0 && speed_ratio <= 1);
        }
    }


    std::vector<std::pair<int, ConfigOptionInts>> overhang_with_fan_speeds = {{100, ConfigOptionInts{0}}};
    if (config.overhangs_dynamic_fan_speed.is_enabled(extruder_id) &&
        overhang_attributes->start_distance_from_prev_layer > 0 &&
        overhang_attributes->end_distance_from_prev_layer > 0) {
        GraphData graph = config.overhangs_dynamic_fan_speed.get_at(extruder_id);
        //interpolate
        assert((overhang_attributes->start_distance_from_prev_layer >= 0 &&
                overhang_attributes->start_distance_from_prev_layer <= 1) ||
               overhang_attributes->start_distance_from_prev_layer == 2);
        assert((overhang_attributes->end_distance_from_prev_layer >= 0 &&
                overhang_attributes->end_distance_from_prev_layer <= 1) ||
               overhang_attributes->end_distance_from_prev_layer == 2);
        fan_speed = std::min(
                     graph.interpolate(100 - 100 * std::min(1.f, overhang_attributes->start_distance_from_prev_layer)),
                     graph.interpolate(100 - 100 * std::min(1.f, overhang_attributes->end_distance_from_prev_layer)));
        assert(fan_speed >= 0 && fan_speed <= 100);
    }
    return {speed_ratio, fan_speed};
}


void apply_overhang_flow(ExtrusionPath &path,
                         const PrintConfig &print_config,
                         const LayerRegion &layer_region,
                         size_t extruder_id) {
    ExtrusionAttributes &attributes = path.attributes_mutable();
    const PrintRegionConfig &region_config = layer_region.region().config();
    const ExtrusionPropertyOverhang *overhang_attributes = path.overhang_attributes();
    if (overhang_attributes == nullptr || overhang_attributes->has_full_overhangs_flow ||
        !region_config.overhangs_flow_ratio.is_enabled() || !region_config.overhangs_dynamic_flow.is_enabled())
        return;
    const double nzl_diam_mm = print_config.nozzle_diameter.get_at(extruder_id);

    GraphData graph = region_config.overhangs_dynamic_flow.value;
    double max_dynamic_distance_mm = region_config.overhangs_width.get_effective_value(nzl_diam_mm);
    // ensure it start at 0%, and ensure it ends at 100%
    if (graph.graph_points[graph.begin_idx].x() != 0) {
        graph.graph_points.insert(graph.graph_points.begin() + graph.begin_idx, {0, 0});
        graph.end_idx++;
    }
    if (graph.graph_points[graph.end_idx - 1].x() != 100) {
        graph.graph_points.insert(graph.graph_points.begin() + graph.end_idx, {100, 100});
        graph.end_idx++;
    }
    graph.graph_points[graph.begin_idx].x() = 0;
    graph.graph_points[graph.end_idx - 1].y() = 100;
    // interpolate
    assert(overhang_attributes->start_distance_from_prev_layer >= 0);
    assert(overhang_attributes->end_distance_from_prev_layer >= 0);
    float extrusion_ratio = 
                    (graph.interpolate(std::min(1., overhang_attributes->start_distance_from_prev_layer / max_dynamic_distance_mm))+
                    graph.interpolate(std::min(1., overhang_attributes->end_distance_from_prev_layer / max_dynamic_distance_mm)))/2;
    assert(extrusion_ratio >= 0 && extrusion_ratio <= 1);

    double max_overhang_mm = 
                    std::max(overhang_attributes->start_distance_from_prev_layer,
                        overhang_attributes->end_distance_from_prev_layer);

    Flow overhang_flow = layer_region.bridging_flow(attributes.extrusion_role().is_external() ? FlowRole::frExternalPerimeter :
                                                                                    FlowRole::frPerimeter);
    if (max_overhang_mm > attributes.width) {
        // create round flow
        const double trigo = 0.25 * PI;
        // mm3_per_mm = (width * width) * 0.25 * PI
        // => width = sqrt(mm3_per_mm / (0.25 * PI))
        const double new_mm3_per_mm = attributes.mm3_per_mm * (1 - extrusion_ratio) +
            overhang_flow.mm3_per_mm() * extrusion_ratio;
        const double new_height = std::sqrt(new_mm3_per_mm / trigo);
        attributes.mm3_per_mm = new_mm3_per_mm;
        attributes.width =new_height;
        attributes.height = new_height;
    } else if (extrusion_ratio > 0) {
        // use flat bottom
        const double trigo = (1. - 0.25 * PI);
        // mm3_per_mm = height * (width - height * (1. - 0.25 * PI))
        // => height = width/(2*trigo) - sqrt( width*width) / (4*trigo*trigo) -  mm3_per_mm/trigo)
        const double old_height = attributes.width / (2 * trigo) -
            std::sqrt(attributes.width * attributes.width / (4 * trigo * trigo) - attributes.mm3_per_mm / trigo);
        assert(is_approx(old_height, attributes.height * 1., 0.01));
        const double new_mm3_per_mm = attributes.mm3_per_mm * (1 - extrusion_ratio) +
            overhang_flow.mm3_per_mm() * extrusion_ratio;
        const double new_height = attributes.width / (2 * trigo) -
            std::sqrt(attributes.width * attributes.width / (4 * trigo * trigo) - new_mm3_per_mm / trigo);
        attributes.mm3_per_mm = new_mm3_per_mm;
        attributes.height = new_height;
    }
}

}} // namespace Slic3r::ExtrusionProcessor
