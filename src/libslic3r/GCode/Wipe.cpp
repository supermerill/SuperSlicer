#include "Wipe.hpp"
#include "../GCode.hpp"

#include <string_view>

#include <Eigen/Geometry>

using namespace std::string_view_literals;

namespace Slic3r::GCode {

void Wipe::init(const PrintConfig &config, const GCodeWriter &writer, const std::vector<uint16_t> &extruders)
{
    this->reset_path();

    // Calculate maximum wipe length to accumulate by the wipe cache.
    // Paths longer than wipe_xy should never be needed for the wipe move.
    double wipe_xy = 0;
    const bool multimaterial = extruders.size() > 1;
    for (uint16_t id : extruders) // != writer.extruders() ?
        if (config.wipe.get_at(id)) {
            // Wipe length to extrusion ratio.
            const double xy_to_e = this->calc_xy_to_e_ratio(writer, id);
            wipe_xy              = std::max(wipe_xy, writer.gcode_config().retract_length.get_at(id) / xy_to_e);
            if (multimaterial)
                wipe_xy = std::max(wipe_xy, writer.gcode_config().retract_length_toolchange.get_at(id) / xy_to_e);
        }

    if (wipe_xy == 0)
        this->disable();
    else
        this->enable(wipe_xy);
}

void Wipe::set_path(const ExtrusionPaths &paths, bool reversed)
{
    this->reset_path();

    if (this->is_enabled() && ! paths.empty()) {
        coord_t wipe_len_max_scaled = scaled(m_wipe_len_max);
        if (reversed) {
            m_path = paths.back().as_polyline().get_arc();
            Geometry::ArcWelder::reverse(m_path);
            int64_t len = Geometry::ArcWelder::estimate_path_length(m_path);
            for (auto it = std::next(paths.rbegin()); len < wipe_len_max_scaled && it != paths.rend(); ++ it) {
                if (it->role().is_bridge())
                    break; // Do not perform a wipe on bridges.
                assert(it->size() >= 2);
                assert(m_path.back().point == it->last_point());
                if (m_path.back().point != it->last_point())
                    // ExtrusionMultiPath is interrupted in some place. This should not really happen.
                    break;
                len += Geometry::ArcWelder::estimate_path_length(it->as_polyline().get_arc());
                m_path.insert(m_path.end(), it->as_polyline().get_arc().rbegin() + 1, it->as_polyline().get_arc().rend());
            }
        } else {
            m_path = std::move(paths.front().as_polyline().get_arc());
            int64_t len = Geometry::ArcWelder::estimate_path_length(m_path);
            for (auto it = std::next(paths.begin()); len < wipe_len_max_scaled && it != paths.end(); ++ it) {
                if (it->role().is_bridge())
                    break; // Do not perform a wipe on bridges.
                assert(it->size() >= 2);
                assert(m_path.back().point == it->first_point());
                if (m_path.back().point != it->first_point())
                    // ExtrusionMultiPath is interrupted in some place. This should not really happen.
                    break;
                len += Geometry::ArcWelder::estimate_path_length(it->as_polyline().get_arc());
                m_path.insert(m_path.end(), it->as_polyline().get_arc().begin() + 1, it->as_polyline().get_arc().end());
            }
        }
    }

    assert(m_path.empty() || m_path.size() > 1);
}

std::pair<double, bool> Wipe::calc_wipe_speed(const GCodeWriter &writer)
{
    double wipe_speed = writer.gcode_config().get_computed_value("travel_speed") * 0.8;
    bool use_wipe_speed = false;
    if (writer.tool_is_extruder() && writer.gcode_config().wipe_speed.get_at(writer.tool()->id()) > 0) {
        wipe_speed = writer.gcode_config().wipe_speed.get_at(writer.tool()->id());
        use_wipe_speed = true;
    }
    return {wipe_speed, use_wipe_speed};
}

std::string Wipe::wipe(GCodeGenerator &gcodegen, bool toolchange)
{
    std::string     gcode;
    const Tool &extruder = *gcodegen.writer().tool();
    static constexpr const std::string_view wipe_retract_comment = "wipe and retract"sv;
    const bool use_firmware_retract = gcodegen.writer().gcode_config().use_firmware_retraction.value;
    
    if (!gcodegen.writer().tool_is_extruder())
        return "";

    // Remaining quantized retraction length.
    double retract_length = extruder.retract_length();
    if (toolchange) {
        retract_length = extruder.retract_length_toolchange();
    } else if (gcodegen.writer().print_region_config() && gcodegen.writer().print_region_config()->print_retract_length.value >= 0) {
        retract_length = gcodegen.writer().print_region_config()->print_retract_length.value;
    }
    retract_length = extruder.retract_to_go(retract_length);
    if (retract_length > 0 && this->has_path()) {
        // Delayed emitting of a wipe start tag.
        bool wiped = false;
        std::pair<double, bool> wipe_speed = this->calc_wipe_speed(gcodegen.writer());
        auto start_wipe = [&wiped, &gcode, &gcodegen, &wipe_speed](){
            if (! wiped) {
                wiped = true;
                gcode += ";" + GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Wipe_Start) + "\n";
                gcode += gcodegen.writer().set_speed(wipe_speed.first * 60, gcodegen.config().gcode_comments ? wipe_speed.second? "wipe_speed"sv : "travel_speed * 0.8"sv : ""sv , gcodegen.enable_cooling_markers() ? ";_WIPE"sv : ""sv);
            }
        };
        const double xy_to_e    = this->calc_xy_to_e_ratio(gcodegen.writer(), extruder.id());
        auto         wipe_linear = [&gcode, &gcodegen, &retract_length, xy_to_e, use_firmware_retract](const Vec2d &prev_quantized, Vec2d &p) {
            Vec2d  p_quantized = gcodegen.writer().get_default_gcode_formatter().quantize(p);
            if (p_quantized == prev_quantized) {
                p = p_quantized; // keep old prev
                return false;
            }
            double segment_length = (p_quantized - prev_quantized).norm();
            // Compute a min dist between point, to avoid going under the precision.
            {
                assert(gcodegen.m_last_width < SCALED_EPSILON);
                coordf_t precision = pow(10, -gcodegen.config().gcode_precision_xyz.value) * 1.5;
                precision = std::max(precision, coordf_t(SCALED_EPSILON * 10));
                if (gcodegen.config().resolution.value > 0) {
                    precision = std::max(precision, scale_d(gcodegen.config().resolution.value));
                } else {
                    precision = std::max(precision, scale_d(gcodegen.m_last_width / 10));
                }
                if (segment_length < precision) {
                    p = p_quantized; // keep old prev
                    return false;
                }
            }
            // Quantize E axis as it is to be extruded as a whole segment.
            double dE = gcodegen.writer().get_default_gcode_formatter().quantize_e(xy_to_e * segment_length);
            bool   done = false;
            if (dE > retract_length - EPSILON) {
                if (dE > retract_length + EPSILON)
                    // Shorten the segment.
                    p = gcodegen.writer().get_default_gcode_formatter().quantize(Vec2d(prev_quantized + (p - prev_quantized) * (retract_length / dE)));
                else
                    p = p_quantized;
                dE   = retract_length;
                done = true;
            } else
                p = p_quantized;
            gcode += gcodegen.writer().extrude_to_xy(p, use_firmware_retract?0:-dE, wipe_retract_comment);
            retract_length -= dE;
            return done;
        };
        auto         wipe_arc = [&gcode, &gcodegen, &retract_length, xy_to_e, use_firmware_retract, &wipe_linear](
            const Vec2d &prev_quantized, Vec2d &p, double radius_in, const bool ccw) {
            Vec2d  p_quantized = gcodegen.writer().get_default_gcode_formatter().quantize(p);
            if (p_quantized == prev_quantized) {
                p = p_quantized;
                return false;
            }
            // Use the exact radius for calculating the IJ values, no quantization.
            double radius = radius_in;
            if (radius == 0)
                // Degenerated arc after quantization. Process it as if it was a line segment.
                return wipe_linear(prev_quantized, p);
            Vec2d  center = Geometry::ArcWelder::arc_center(prev_quantized.cast<double>(), p_quantized.cast<double>(), double(radius), ccw);
            float  angle  = Geometry::ArcWelder::arc_angle(prev_quantized.cast<double>(), p_quantized.cast<double>(), double(radius));
            assert(angle > 0);
            double segment_length = angle * std::abs(radius);
            double dE = gcodegen.writer().get_default_gcode_formatter().quantize_e(xy_to_e * segment_length);
            bool   done = false;
            if (dE > retract_length - EPSILON) {
                if (dE > retract_length + EPSILON) {
                    // Shorten the segment. Recalculate the arc from the unquantized end coordinate.
                    center = Geometry::ArcWelder::arc_center(prev_quantized.cast<double>(), p.cast<double>(), double(radius), ccw);
                    angle = Geometry::ArcWelder::arc_angle(prev_quantized.cast<double>(), p.cast<double>(), double(radius));
                    segment_length = angle * std::abs(radius);
                    dE = xy_to_e * segment_length;
                    p = gcodegen.writer().get_default_gcode_formatter().quantize(
                            Vec2d(center + Eigen::Rotation2D((ccw ? angle : -angle) * (retract_length / dE)) * (prev_quantized - center)));
                } else
                    p = p_quantized;
                dE   = retract_length;
                done = true;
            } else
                p = p_quantized;
            assert(dE > 0);
            {
                // Calculate quantized IJ circle center offset.
                Vec2d ij = gcodegen.writer().get_default_gcode_formatter().quantize(Vec2d(center - prev_quantized));
                if (ij == Vec2d::Zero())
                    // Degenerated arc after quantization. Process it as if it was a line segment.
                    return wipe_linear(prev_quantized, p);
                // The arc is valid.
                gcode += gcodegen.writer().extrude_arc_to_xy(
                    p, ij, ccw, use_firmware_retract?0:-dE, wipe_retract_comment);
            }
            retract_length -= dE;
            return done;
        };
        // Start with the current position, which may be different from the wipe path start in case of loop clipping.
        Vec2d prev = gcodegen.point_to_gcode_quantized(gcodegen.last_pos());
        auto  it   = this->path().begin();
        Vec2d p    = gcodegen.point_to_gcode(it->point + m_offset);
        ++ it;
        bool done = false;
        if (p != prev) {
            start_wipe();
            done = wipe_linear(prev, p);
        }
        if (! done) {
            prev = p;
            auto end = this->path().end();
            for (; it != end; ++ it) {
                p = gcodegen.point_to_gcode(it->point + m_offset);
                if (p != prev) {
                    start_wipe();
                    if (it->linear() ?
                        wipe_linear(prev, p) :
                        wipe_arc(prev, p, unscaled<double>(it->radius), it->ccw()))
                        break;
                    prev = p;
                }
            }
        }
        if (wiped) {
            // add tag for processor
            assert(p == gcodegen.writer().get_default_gcode_formatter().quantize(p));
            gcode += ";" + GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Wipe_End) + "\n";
            gcodegen.set_last_pos(gcodegen.gcode_to_point(p));
        }
    }

    // Prevent wiping again on the same path.
    this->reset_path();
    return gcode;
}


// Returns true if the smooth path is longer than a threshold.
bool longer_than(const ExtrusionPaths &paths, double length)
{
    for (const ExtrusionPath &path : paths) {
        for (auto it = std::next(path.as_polyline().get_arc().begin()); it != path.as_polyline().get_arc().end(); ++it) {
            length -= Geometry::ArcWelder::segment_length<double>(*std::prev(it), *it);
            if (length < 0)
                return true;
        }
    }
    return length < 0;
}


// 
// Length of a smooth path.
//
std::optional<Point> sample_path_point_at_distance_from_start(const ExtrusionPaths &paths, double distance)
{
    if (distance >= 0) {
        for (const ExtrusionPath &path : paths) {
            auto it  = path.as_polyline().get_arc().begin();
            auto end = path.as_polyline().get_arc().end();
            Point prev_point = it->point;
            for (++ it; it != end; ++ it) {
                Point point = it->point;
                if (it->linear()) {
                    // Linear segment
                    Vec2d  v    = (point - prev_point).cast<double>();
                    double lsqr = v.squaredNorm();
                    if (lsqr > sqr(distance))
                        return std::make_optional<Point>(prev_point + (v * (distance / sqrt(lsqr))).cast<coord_t>());
                    distance -= sqrt(lsqr);
                } else {
                    // Circular segment
                    float angle = Geometry::ArcWelder::arc_angle(prev_point.cast<float>(), point.cast<float>(), it->radius);
                    double len = std::abs(it->radius) * angle;
                    if (len > distance) {
                        // Rotate the segment end point in reverse towards the start point.
                        return std::make_optional<Point>(prev_point.rotated(- angle * (distance / len),
                            Geometry::ArcWelder::arc_center(prev_point.cast<float>(), point.cast<float>(), it->radius, it->ccw()).cast<coord_t>()));
                    }
                    distance -= len;
                }
                if (distance < 0)
                    return std::make_optional<Point>(point);
                prev_point = point;
            }
        }
    }
    // Failed.
    return {};
}

std::optional<Point> sample_path_point_at_distance_from_end(const ExtrusionPaths &paths, double distance)
{
    if (distance >= 0) {
        for (const ExtrusionPath& path : paths) {
            auto it = path.as_polyline().get_arc().begin();
            auto end = path.as_polyline().get_arc().end();
            Point prev_point = it->point;
            for (++it; it != end; ++it) {
                Point point = it->point;
                if (it->linear()) {
                    // Linear segment
                    Vec2d  v = (point - prev_point).cast<double>();
                    double lsqr = v.squaredNorm();
                    if (lsqr > sqr(distance))
                        return std::make_optional<Point>(prev_point + (v * (distance / sqrt(lsqr))).cast<coord_t>());
                    distance -= sqrt(lsqr);
                }
                else {
                    // Circular segment
                    float angle = Geometry::ArcWelder::arc_angle(prev_point.cast<float>(), point.cast<float>(), it->radius);
                    double len = std::abs(it->radius) * angle;
                    if (len > distance) {
                        // Rotate the segment end point in reverse towards the start point.
                        return std::make_optional<Point>(prev_point.rotated(-angle * (distance / len),
                            Geometry::ArcWelder::arc_center(prev_point.cast<float>(), point.cast<float>(), it->radius, it->ccw()).cast<coord_t>()));
                    }
                    distance -= len;
                }
                if (distance < 0)
                    return std::make_optional<Point>(point);
                prev_point = point;
            }
        }
    }
    // Failed.
    return {};
}

// Make a little move inwards before leaving loop after path was extruded,
// thus the current extruder position is at the end of a path and the path
// may not be closed in case the loop was clipped to hide a seam.
std::optional<Point> wipe_hide_seam(const ExtrusionPaths &paths, bool is_hole, double wipe_length)
{
    assert(! paths.empty());
    assert(paths.front().size() >= 2);
    assert(paths.back().size() >= 2);

    // Heuristics for estimating whether there is a chance that the wipe move will fit inside a small perimeter
    // or that the wipe move direction could be calculated with reasonable accuracy.
    if (longer_than(paths, 2.5 * wipe_length)) {
        // The print head will be moved away from path end inside the island.
        Point p_current = paths.back().last_point();//paths.back().path.back().point;
        Point p_next = paths.front().first_point();//paths.front().path.front().point;
        Point p_prev;
        {
            // Is the seam hiding gap large enough already?
            double l = wipe_length - (p_next - p_current).cast<double>().norm();
            if (l > 0) {
                // Not yet.
                std::optional<Point> n = sample_path_point_at_distance_from_start(paths, l);
                assert(n);
                if (! n)
                    // Wipe move cannot be calculated, the loop is not long enough. This should not happen due to the longer_than() test above.
                    return {};
            }
            if (std::optional<Point> p = sample_path_point_at_distance_from_end(paths, wipe_length); p)
                p_prev = *p;
            else
                // Wipe move cannot be calculated, the loop is not long enough. This should not happen due to the longer_than() test above.
                return {};
        }
        // Detect angle between last and first segment.
        // The side depends on the original winding order of the polygon (left for contours, right for holes).
        double angle_inside = angle_ccw(p_next - p_current, p_prev - p_current);
        assert(angle_inside >= -M_PI && angle_inside <= M_PI);
        // 3rd of this angle will be taken, thus make the angle monotonic before interpolation.
        if (is_hole) {
            if (angle_inside > 0)
                angle_inside -= 2.0 * M_PI;
        } else {
            if (angle_inside < 0)
                angle_inside += 2.0 * M_PI;
        }
        // Rotate the forward segment inside by 1/3 of the wedge angle.
        auto v_rotated = Eigen::Rotation2D(angle_inside) * (p_next - p_current).cast<double>().normalized();
        return std::make_optional<Point>(p_current + (v_rotated * wipe_length).cast<coord_t>());
    }

    return {};
}

// Superslicer methods
//
//void Wipe::append(const Point &p)
//{
//    assert(this->path.empty() || !this->path.last_point().coincides_with_epsilon(p));
//    this->path.append(p);
//}
//
//void Wipe::append(const Polyline &poly)
//{
//    assert(!poly.empty());
//    if (!this->path.empty() && path.last_point().coincides_with_epsilon(poly.first_point())) {
//        int copy_start_idx = 0;
//        while (copy_start_idx < poly.size() && poly.points[copy_start_idx].distance_to(this->path.last_point()) < SCALED_EPSILON) {
//            copy_start_idx++;
//        }
//        if (copy_start_idx >= poly.size())
//            return;
//        assert(!this->path.last_point().coincides_with_epsilon(poly.points[copy_start_idx]));
//        this->path.append(poly.points.begin() + copy_start_idx, poly.points.end());
//    } else {
//        this->path.append(poly);
//    }
//}
//
//void Wipe::set(const Polyline &p)
//{
//    path = p;
//}
} // namespace Slic3r::GCode
