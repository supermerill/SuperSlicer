///|/ Copyright (c) Prusa Research 2016 - 2021 Vojtěch Bubník @bubnikv
///|/ Copyright (c) Slic3r 2014 - 2016 Alessandro Ranellucci @alranel
///|/ Copyright (c) 2015 Maksim Derbasov @ntfshard
///|/
///|/ PrusaSlicer is released under the terms of the AGPLv3 or higher
///|/
#include "BridgeDetector.hpp"
#include "ClipperUtils.hpp"
#include "Geometry.hpp"
#include <algorithm>

namespace Slic3r {

BridgeDetector::BridgeDetector(ExPolygon         _expolygon,
                               const ExPolygons &_lower_slices,
                               coord_t           _extrusion_spacing,
                               coord_t           _precision,
                               int               layer_idx)
    :
    // The original infill polygon, not inflated.
    expolygons(expolygons_owned),
    // All surfaces of the object supporting this region.
    lower_slices(_lower_slices),
    spacing(_extrusion_spacing),
    precision(_precision),
    layer_id(layer_idx)
{
    this->expolygons_owned.push_back(std::move(_expolygon));
    initialize();
}

BridgeDetector::BridgeDetector(const ExPolygons &_expolygons,
                               const ExPolygons &_lower_slices,
                               coord_t           _extrusion_spacing,
                               coord_t           _precision,
                               int               layer_idx)
    : 
    // The original infill polygon, not inflated.
    expolygons(_expolygons),
    // All surfaces of the object supporting this region.
    lower_slices(_lower_slices),
    spacing(_extrusion_spacing),
    precision(_precision),
    layer_id(layer_idx)
{
    initialize();
}

void BridgeDetector::initialize()
{
    // 2 degrees stepping
    this->resolution = PI/(90); 
    // output angle not known
    this->angle = -1.;

    // Outset our bridge by an arbitrary amout; we'll use this outer margin for detecting anchors.
    Polygons grown = offset(this->expolygons, float(this->spacing * 0.5), ClipperLib::JoinType::jtMiter);
    
    //remove bits that shoudln't be here, but are due to the grow + clip
    //get the unsupported out-of-part section (if any)
    ExPolygons union_lower_slices = union_safety_offset_ex(this->lower_slices);
    ExPolygons part_areas = union_lower_slices;
    append(part_areas, this->expolygons);
    part_areas = union_safety_offset_ex(part_areas);
    ExPolygons out_of_part = diff_ex(grown, part_areas);
    // then grow it
    out_of_part = offset_ex(out_of_part, float(this->spacing * 0.55), ClipperLib::JoinType::jtMiter);
    // remove it from grow
    grown = diff(grown, out_of_part);
    //finish growing
    grown = offset(grown, float(this->spacing * 0.55f), ClipperLib::JoinType::jtMiter, 6);

    // Detect possible anchoring edges of this bridging region.
    // Detect what edges lie on lower slices by turning bridge contour and holes
    // into polylines and then clipping them with each lower slice's contour.
    // Currently _edges are only used to set a candidate direction of the bridge (see bridge_direction_candidates()).
    Polygons contours;
    contours.reserve(this->lower_slices.size());
    for (const ExPolygon &expoly : this->lower_slices)
        contours.push_back(expoly.contour);
    this->_edges = intersection_pl(to_polylines(grown), contours);
    
    #ifdef SLIC3R_DEBUG
    printf("  bridge has %zu support(s)\n", this->_edges.size());
    #endif
    
    // detect anchors as intersection between our bridge expolygon and the lower slices
    // safety offset required to avoid Clipper from detecting empty intersection while Boost actually found some edges
    this->_anchor_regions = intersection_ex(grown, union_lower_slices);
    
    /*
    if (0) {
        require "Slic3r/SVG.pm";
        Slic3r::SVG::output("bridge.svg",
            expolygons      => [ $self->expolygon ],
            red_expolygons  => $self->lower_slices,
            polylines       => $self->_edges,
        );
    }
    */
}

bool BridgeDetector::detect_angle(double bridge_direction_override)
{
    if (this->_edges.empty() || this->_anchor_regions.empty()) 
        // The bridging region is completely in the air, there are no anchors available at the layer below.
        return false;

    std::vector<BridgeDirection> candidates;
    if (bridge_direction_override == 0.) {
        candidates = bridge_direction_candidates();
    } else
        candidates.emplace_back(BridgeDirection(bridge_direction_override));
    
    /*  Outset the bridge expolygon by half the amount we used for detecting anchors;
        we'll use this one to clip our test lines and be sure that their endpoints
        are inside the anchors and not on their contours leading to false negatives. */
    Polygons clip_area = offset(this->expolygons, 0.5f * float(this->spacing));
    // union with offseted anchor before un-offset to get the good clip area with anchor added.
    ExPolygons unoffset_clip = offset_ex(this->_anchor_regions, 0.5f * float(this->spacing));
    for (Polygon &poly : clip_area) {
        unoffset_clip.emplace_back(poly);
    }
    unoffset_clip = union_ex(unoffset_clip);
    unoffset_clip = offset_ex(unoffset_clip, -0.5f * float(this->spacing));
    // now clip the clip with not-offset merged anchor + expolygons, so it's enlarged only inside the anchor.
    clip_area = intersection(unoffset_clip, clip_area);

    
    /*  we'll now try several directions using a rudimentary visibility check:
        bridge in several directions and then sum the length of lines having both
        endpoints within anchors */
        
    bool have_coverage = false;
    for (size_t i_angle = 0; i_angle < candidates.size(); ++ i_angle)
    {
        const double angle = candidates[i_angle].angle;
        Lines lines;
        {
            // Get an oriented bounding box around _anchor_regions.
            BoundingBox bbox = get_extents_rotated(this->_anchor_regions, - angle);
            // Cover the region with line segments.
            lines.reserve((bbox.max.y() - bbox.min.y() + this->spacing - SCALED_EPSILON) / this->spacing);
            double s = sin(angle);
            double c = cos(angle);
            // As The lines be spaced half the line width from the edge
            // FIXME: some of the test cases may fail. Need to adjust the test cases
            for (coord_t y = bbox.min.y() + this->spacing / 2; y <= bbox.max.y(); y += this->spacing)
            //for (coord_t y = bbox.min.y(); y <= bbox.max.y(); y += this->spacing) //this is the old version
                lines.push_back(Line(
                    Point((coord_t)round(c * bbox.min.x() - s * y), (coord_t)round(c * y + s * bbox.min.x())),
                    Point((coord_t)round(c * bbox.max.x() - s * y), (coord_t)round(c * y + s * bbox.max.x()))));
        }

        //create boundingbox for anchor regions
        std::vector<BoundingBox> anchor_bb;
        for (ExPolygon& poly : this->_anchor_regions) {
            anchor_bb.emplace_back(poly.contour.bounding_box());
        }

        //compute stat on line with anchors, and their lengths.
        BridgeDirection& bridge_dir_candidate = candidates[i_angle];
        std::vector<coordf_t> dist_anchored;
        {
            Lines clipped_lines = intersection_ln(lines, clip_area);
            for (size_t i = 0; i < clipped_lines.size(); ++i) {
                // this can be called 100 000 time per detect_angle, please optimise
                const Line &line = clipped_lines[i];
                bool good_line = false;
                bool fake_bridge = false;
                coordf_t len = line.length();
                // check if the line isn't too long
                if (good_line && max_bridge_length > 0 && len > max_bridge_length) {
                    good_line = false;
                } else {
                    // is anchored?
                    size_t line_a_anchor_idx = -1;
                    size_t line_b_anchor_idx = -1;
                    for (int i = 0; i < _anchor_regions.size(); ++i) {
                        ExPolygon &  poly   = this->_anchor_regions[i];
                        BoundingBox &polybb = anchor_bb[i];
                        if (polybb.contains(line.a) &&
                            poly.contains(
                                line.a)) { // using short-circuit evaluation to test boundingbox and only then the other
                            line_a_anchor_idx = i;
                        }
                        if (polybb.contains(line.b) &&
                            poly.contains(
                                line.b)) { // using short-circuit evaluation to test boundingbox and only then the other
                            line_b_anchor_idx = i;
                        }
                        if (line_a_anchor_idx < clipped_lines.size() && line_b_anchor_idx < clipped_lines.size())
                            break;
                    }
                    // check if the anchor search has been successful
                    // note: this 'if' isn't very effective (culls ~ 10% on a benchy) but it's almost free to compute
                    if ((line_a_anchor_idx < clipped_lines.size()) & (line_b_anchor_idx < clipped_lines.size())) {
                        good_line = true;
                        // test if it's not a fake bridge
                        if (line_a_anchor_idx == line_b_anchor_idx) {
                            fake_bridge = true;
                            // check that the line go out of the anchor into the briding area
                            // don't call intersection_ln here, as even if we succeed to limit the number of
                            // candidates to ~100, here we can have hundreds of lines, so that means dozen of
                            // thousands of calls (or more)! add some points (at least the middle) to test, it's quick
                            Point middle_point = line.midpoint();
                            for (int i = 0; i < _anchor_regions.size(); ++i) {
                                ExPolygon &  poly   = this->_anchor_regions[i];
                                BoundingBox &polybb = anchor_bb[i];
                                if (!polybb.contains(middle_point) ||
                                    !poly.contains(middle_point)) { // using short-circuit evaluation to test
                                                                    // boundingbox and only then the other
                                    fake_bridge = false;
                                    goto stop_fake_bridge_test;
                                }
                            }
                            // try with rigth & left
                            if (fake_bridge && len > this->spacing * 4) {
                                Vector normal = line.normal();
                                normal.normalize();
                                normal *= coordf_t(spacing / 2);
                                Point middle_point_right = line.midpoint() + normal;
                                Point middle_point_left  = line.midpoint() - normal;
                                for (int i = 0; i < _anchor_regions.size(); ++i) {
                                    ExPolygon &  poly   = this->_anchor_regions[i];
                                    BoundingBox &polybb = anchor_bb[i];
                                    if (!poly.contains(middle_point_right) || !poly.contains(middle_point_left)) {
                                        fake_bridge = false;
                                        goto stop_fake_bridge_test;
                                    }
                                }
                            }
                            // if still bad, the line is long enough to warrant two more test point? (1/2000 on a benchy)
                            if (fake_bridge && len > this->spacing * 10) {
                                // now test with to four more points
                                Vector normal = line.normal();
                                normal.normalize();
                                normal *= coordf_t(spacing / 2);
                                Points pts;
                                pts.push_back((line.a + middle_point) / 2 + normal);
                                pts.push_back((line.a + middle_point) / 2 - normal);
                                pts.push_back((line.b + middle_point) / 2 + normal);
                                pts.push_back((line.b + middle_point) / 2 - normal);
                                for (int i = 0; i < _anchor_regions.size(); ++i) {
                                    ExPolygon &  poly   = this->_anchor_regions[i];
                                    BoundingBox &polybb = anchor_bb[i];
                                    for (Point &pt : pts)
                                        if (!polybb.contains(pt) ||
                                            !poly.contains(pt)) { // using short-circuit evaluation to test
                                                                  // boundingbox and only then the other
                                            fake_bridge = false;
                                            goto stop_fake_bridge_test;
                                        }
                                }
                            }
                            // If the line is still bad and is a long one, use the more costly intersection_ln. This
                            // case is rare enough to swallow the cost. (1/10000 on a benchy)
                            if (fake_bridge && len > this->spacing * 40) {
                                // now test with intersection_ln
                                Lines lines = intersection_ln(line, to_polygons(this->_anchor_regions));
                                // if < 2, not anchored at both end
                                fake_bridge = lines.size() < 2;
                            }
                        }
                    }
stop_fake_bridge_test: ;
                }
                if (good_line && fake_bridge)
                    bridge_dir_candidate.nb_lines_fake_bridge++;
                if (good_line) {
                    // This line could be anchored at both side and goes over the void to bridge it in its middle.
                    //store stats
                    if (!fake_bridge) {
                        bridge_dir_candidate.total_length_anchored += len;
                        bridge_dir_candidate.max_length_anchored = std::max(bridge_dir_candidate.max_length_anchored, len);
                    }
                    bridge_dir_candidate.nb_lines_anchored++;
                    dist_anchored.push_back(len);
                } else {
                    // this line could NOT be anchored.
                    bridge_dir_candidate.total_length_free += len;
                    bridge_dir_candidate.max_length_free = std::max(bridge_dir_candidate.max_length_free, len);
                    bridge_dir_candidate.nb_lines_free++;
                }
            }        
        }
        if (bridge_dir_candidate.total_length_anchored == 0. || bridge_dir_candidate.nb_lines_anchored == 0) {
            continue;
        } else {
            have_coverage = true;
            // compute median
            if (!dist_anchored.empty()) {
                std::sort(dist_anchored.begin(), dist_anchored.end());
                bridge_dir_candidate.median_length_anchor = dist_anchored[dist_anchored.size() / 2];
            }


            // size is 20%
        }
    }

    // if no direction produced coverage, then there's no bridge direction ?
    if (!have_coverage) {
        //try again to choose the least worse
        // use only poly contour angles
        if (bridge_direction_override == 0.) {
            candidates = bridge_direction_candidates(true);
        } else
            candidates.emplace_back(BridgeDirection(bridge_direction_override));
        for (size_t i_angle = 0; i_angle < candidates.size(); ++i_angle)
        {
            const double angle = candidates[i_angle].angle;
            //use the whole polygon
            Lines lines;
            {
                // Get an oriented bounding box around _anchor_regions.
                BoundingBox bbox = get_extents_rotated(clip_area, -angle);
                // Cover the region with line segments.
                lines.reserve((bbox.max.y() - bbox.min.y() + this->spacing - SCALED_EPSILON) / this->spacing);
                double s = sin(angle);
                double c = cos(angle);
                // The lines be spaced half the line width from the edge
                for (coord_t y = bbox.min.y() + this->spacing / 2; y <= bbox.max.y(); y += this->spacing)
                    lines.push_back(Line(
                        Point((coord_t)round(c * bbox.min.x() - s * y), (coord_t)round(c * y + s * bbox.min.x())),
                        Point((coord_t)round(c * bbox.max.x() - s * y), (coord_t)round(c * y + s * bbox.max.x()))));
            }
            //compute stat on line with anchors, and their lengths.
            BridgeDirection& c = candidates[i_angle];
            std::vector<coordf_t> dist_anchored;
            {
                Lines clipped_lines = intersection_ln(lines, clip_area);
                for (size_t i = 0; i < clipped_lines.size(); ++i) {
                    const Line& line = clipped_lines[i];
                    if (expolygons_contain(this->_anchor_regions, line.a) || expolygons_contain(this->_anchor_regions, line.b)) {
                        // This line has one anchor (or is totally anchored)
                        coordf_t len = line.length();
                        //store stats
                        c.total_length_anchored += len;
                        c.max_length_anchored = std::max(c.max_length_anchored, len);
                        c.nb_lines_anchored++;
                        dist_anchored.push_back(len);
                    } else {
                        // this line could NOT be anchored.
                        coordf_t len = line.length();
                        c.total_length_free += len;
                        c.max_length_free = std::max(c.max_length_free, len);
                        c.nb_lines_free++;
                    }
                }
            }
            if (c.total_length_anchored == 0. || c.nb_lines_anchored == 0) {
                continue;
            } else {
                have_coverage = true;
                // compute median
                if (!dist_anchored.empty()) {
                    std::sort(dist_anchored.begin(), dist_anchored.end());
                    c.median_length_anchor = dist_anchored[dist_anchored.size() / 2];
                }


                // size is 20%
            }
        }
    }

    // if no direction produced coverage, then there's no bridge direction
    if (!have_coverage)
        return false;

    //compute global stat (max & min median & max length)
    std::vector<coordf_t> all_median_length;
    std::vector<coordf_t> all_max_length;
    for (BridgeDirection &c : candidates) {
        all_median_length.push_back(c.median_length_anchor);
        all_max_length.push_back(c.max_length_anchored);
    }
    std::sort(all_median_length.begin(), all_median_length.end());
    std::sort(all_max_length.begin(), all_max_length.end());
    coordf_t median_max_length = all_max_length[all_max_length.size() / 2];
    coordf_t min_max_length = all_max_length.front();
    coordf_t max_max_length = all_max_length.back();
    coordf_t median_median_length = all_median_length[all_median_length.size() / 2];
    coordf_t min_median_length = all_median_length.front();
    coordf_t max_median_length = all_median_length.back();

    //compute individual score
    for (BridgeDirection& c : candidates) {
        c.coverage = 0;
        //ratio_anchored is 70% of the score
        double ratio_anchored = c.total_length_anchored / (c.total_length_anchored + c.total_length_free);
        c.coverage = 70 * ratio_anchored;
        //median is 15% (and need to invert it)
        double ratio_median = 1 - double(c.median_length_anchor - min_median_length) / (double)std::max(1., max_median_length - min_median_length);
        c.coverage += 15 * ratio_median;
        //max is 15 % (and need to invert it)
        double ratio_max = 1 - double(c.max_length_anchored - min_max_length) / (double)std::max(1., max_max_length - min_max_length);
        c.coverage += 15 * ratio_max;
        //bonus for perimeter dir
        if (c.along_perimeter_length > 0)
            c.coverage += 5;

    }
    
    // if any other direction is within extrusion width of coverage, prefer it if shorter
    // shorter = shorter max length, or if in espilon (10) range, the shorter mean length.
    // TODO: There are two options here - within width of the angle with most coverage, or within width of the currently perferred?
    size_t i_best = 0;
    for (size_t i = 1; i < candidates.size(); ++ i)
        if (candidates[i].coverage > candidates[i_best].coverage)
            i_best = i;
        else if (candidates[i].coverage > candidates[i_best].coverage)
            i_best = i;

    this->angle = candidates[i_best].angle;
    if (this->angle >= PI)
        this->angle -= PI;

    #ifdef SLIC3R_DEBUG
    printf("  Optimal infill angle is %d degrees\n", (int)Slic3r::Geometry::rad2deg(this->angle));
    #endif

    return true;
}

std::vector<BridgeDetector::BridgeDirection> BridgeDetector::bridge_direction_candidates(bool only_from_polygon) const
{
    std::vector<BridgeDirection> angles;
    // we test angles according to configured resolution
    if (!only_from_polygon)
        for (int i = 0; i <= PI/this->resolution; ++i)
            angles.emplace_back(i * this->resolution);
    
    // we also test angles of each bridge contour
    {
        Lines lines = to_lines(this->expolygons);
        //if many lines, only takes the bigger ones.
        float mean_sqr_size = 0;
        if (lines.size() > 200) {
            for (int i = 0; i < 200; i++) {
                mean_sqr_size += (float)lines[i].a.distance_to_square(lines[i].b);
            }
            mean_sqr_size /= 200;
            for (Lines::const_iterator line = lines.begin(); line != lines.end(); ++line) {
                float dist_sqr = line->a.distance_to_square(line->b);
                if (dist_sqr > mean_sqr_size)
                    angles.emplace_back(line->direction(), dist_sqr);
            }
        }else
            for (Lines::const_iterator line = lines.begin(); line != lines.end(); ++line)
                angles.emplace_back(line->direction(), line->a.distance_to_square(line->b));
    }
    
    /*  we also test angles of each open supporting edge
        (this finds the optimal angle for C-shaped supports) */
    for (const Polyline &edge : this->_edges)
        if (edge.first_point() != edge.last_point())
            angles.emplace_back(Line(edge.first_point(), edge.last_point()).direction());
    
    // remove duplicates
    std::sort(angles.begin(), angles.end(), [](const BridgeDirection& bt1, const BridgeDirection& bt2) { return bt1.angle < bt2.angle; });

    //first delete angles too close to an angle from a perimeter  
    for (size_t i = 1; i < angles.size(); ++i) {
        if (angles[i - 1].along_perimeter_length > 0 && angles[i].along_perimeter_length == 0)
            if (Slic3r::Geometry::directions_parallel(angles[i].angle, angles[i - 1].angle, this->resolution)) {
                angles.erase(angles.begin() + i);
                --i;
                continue;
            }
        if (angles[i].along_perimeter_length > 0 && angles[i - 1].along_perimeter_length == 0)
            if (Slic3r::Geometry::directions_parallel(angles[i].angle, angles[i - 1].angle, this->resolution)) {
                angles.erase(angles.begin() + (i-1));
                --i;
                continue;
            }
    }
    //then delete angle to close to each other (high resolution)
    double min_resolution = this->resolution / 8;
    for (size_t i = 1; i < angles.size(); ++i) {
        if (Slic3r::Geometry::directions_parallel(angles[i].angle, angles[i - 1].angle, min_resolution)) {
            // keep the longest of the two.
            if (angles[i].along_perimeter_length < angles[i - 1].along_perimeter_length) {
                angles.erase(angles.begin() + i);
                --i;
            } else {
                angles.erase(angles.begin() + (i-1));
                --i;
            }
        }
    }
    //then, if too much angles, delete more
    while (angles.size() > 200) {
        min_resolution *= 2;
        for (size_t i = 1; i < angles.size(); ++i) {
            if (Slic3r::Geometry::directions_parallel(angles[i].angle, angles[i - 1].angle, min_resolution)) {
                // keep the longest of the two.
                if (angles[i].along_perimeter_length < angles[i - 1].along_perimeter_length) {
                    angles.erase(angles.begin() + i);
                    --i;
                } else {
                    angles.erase(angles.begin() + (i - 1));
                    --i;
                }
            }
        }
    }
    /*  compare first value with last one and remove the greatest one (PI) 
        in case they are parallel (PI, 0) */
    if (angles.size() > 1 && Slic3r::Geometry::directions_parallel(angles.front().angle, angles.back().angle, min_resolution))
        angles.pop_back();

    return angles;
}

/*
static void get_trapezoids(const ExPolygon &expoly, Polygons* polygons) const
{
    ExPolygons expp;
    expp.push_back(expoly);
    boost::polygon::get_trapezoids(*polygons, expp);
}

void ExPolygon::get_trapezoids(ExPolygon clone, Polygons* polygons, double angle) const
{
    clone.rotate(PI/2 - angle, Point(0,0));
    clone.get_trapezoids(polygons);
    for (Polygons::iterator polygon = polygons->begin(); polygon != polygons->end(); ++polygon)
        polygon->rotate(-(PI/2 - angle), Point(0,0));
}
*/

void get_lines(const ExPolygon& expoly, std::vector<Line> &lines, coord_t spacing, int layer_id, ExPolygons anchorage)
{

    // get all points of this ExPolygon
    Points pp = to_points(expoly);

    if (pp.empty()) return;

    // build our bounding box
    BoundingBox bb(pp);

    // get all x coordinates
    coord_t min_x = pp[0].x(), max_x = pp[0].x();
    std::vector<coord_t> xx;
    for (Points::const_iterator p = pp.begin(); p != pp.end(); ++p) {
        if (min_x > p->x()) min_x = p->x();
        if (max_x < p->x()) max_x = p->x();
    }
    for (coord_t x = min_x; x < max_x - (spacing / 2); x += spacing) {
        xx.push_back(x);
    }
    xx.push_back(max_x);
    //std::sort(xx.begin(), xx.end());

    // find trapezoids by looping from first to next-to-last coordinate
    coord_t prev_x = xx.front() - SCALED_EPSILON;
    for (std::vector<coord_t>::const_iterator x = xx.begin(); x != xx.end(); ++x) {
        if (*x == prev_x) continue;
        prev_x = *x;
        
        lines.emplace_back(Point(*x, bb.min(1) - spacing / 2), Point(*x, bb.max(1) + spacing / 2));
        assert(lines.back().a.x() == lines.back().b.x());
        assert(lines.back().a.y() < lines.back().b.y());
    }
}

Polygons BridgeDetector::coverage(double angle) const
{
    if (angle == -1)
        angle = this->angle;

    Polygons      covered;
    const coord_t covered_offset = this->precision / 2 + SCALED_EPSILON / 2;

    if (angle != -1) {
        // Get anchors, convert them to Polygons and rotate them.
        ExPolygons anchors = this->_anchor_regions;
        expolygons_rotate(anchors, PI / 2.0 - angle);

        for (ExPolygon unsupported : this->expolygons) {
            // Clone our expolygon and rotate it so that we work with vertical lines.
            unsupported.rotate(PI / 2.0 - angle);
            // Outset the bridge expolygon by half the amount we used for detecting anchors;
            // we'll use this one to generate our trapezoids and be sure that their vertices
            // are inside the anchors and not on their contours leading to false negatives.
            ExPolygons unsupported_bigger = offset_ex(unsupported, 0.5f * float(this->spacing));
            assert(unsupported_bigger.size() == 1); // growing don't split
            ExPolygons small_anchors = intersection_ex({unsupported_bigger.front()}, anchors);
            unsupported_bigger       = small_anchors;
            unsupported_bigger.push_back(unsupported);
            unsupported_bigger = union_safety_offset_ex(unsupported_bigger);
            // now unsupported_bigger is unsupported but with a little extra inside the anchors
            // clean it up if needed (remove bits unlinked to 'unsupported'
            if (unsupported_bigger.size() > 1) {
                double biggest_area = 0;
                for (auto it = unsupported_bigger.begin(); it != unsupported_bigger.end(); ++it) {
                    biggest_area = std::max(biggest_area, it->area());
                }
                auto it = unsupported_bigger.begin();
                while (it != unsupported_bigger.end()) {
                    if (it->area() >= biggest_area - 1) {
                        ++it;
                    } else {
                        it = unsupported_bigger.erase(it);
                    }
                }
            }
            assert(unsupported_bigger.size() == 1);
            {
                std::vector<Line> support_lines;
                get_lines(unsupported_bigger.front(), support_lines, this->precision, layer_id, anchors);
                std::vector<Lines> lines_checked;
                for (Line &line : support_lines) {
                    lines_checked.emplace_back();
                    // intersection to have the printable lines
                    Polylines pls = intersection_pl({Polyline{line.a, line.b}}, unsupported_bigger);
                    for (Polyline &pl : pls) {
                        // you can't add point with a cut
                        assert(pl.size() == 2);
                        // check if the line is anchored
                        bool has_a = false, has_b = false;
                        for (ExPolygon anchor : anchors) {
                            has_a = has_a || anchor.contains(pl.front());
                            has_b = has_b || anchor.contains(pl.back());
                        }
                        // not both in anchor: bad. discard.
                        if (!has_a || !has_b)
                            continue;
                        lines_checked.back().emplace_back(pl.front(), pl.back());
                    }
                }
                assert(lines_checked.size() == support_lines.size());

                // create polygons inflated by covered_offset from good lines
                for (Lines &lines : lines_checked) {
                    Polygon p;
                    for (Line &l : lines) {
                        covered.emplace_back(Points{Point(l.a.x() - covered_offset, l.a.y() - covered_offset),
                                                    Point(l.a.x() - covered_offset, l.b.y() + covered_offset),
                                                    Point(l.a.x() + covered_offset, l.b.y() + covered_offset),
                                                    Point(l.a.x() + covered_offset, l.a.y() - covered_offset)});
                    }
                }
            }
        }

        // Unite the polygons created from lines
        covered = union_(covered);
        // unoffset the polygons, so it doesn't expand into un-printable areas
        covered = offset(covered, -covered_offset);

        // Intersect trapezoids with actual bridge area to remove extra margins and append it to result.
        polygons_rotate(covered, -(PI / 2.0 - angle));
    }
    return covered;
}

/*  This method returns the bridge edges (as polylines) that are not supported
    but would allow the entire bridge area to be bridged with detected angle
    if supported too */
void BridgeDetector::unsupported_edges(double angle, Polylines* unsupported) const
{
    if (angle == -1) angle = this->angle;
    if (angle == -1) return;

    Polygons grown_lower = offset(this->lower_slices, float(this->spacing));

    for (ExPolygons::const_iterator it_expoly = this->expolygons.begin(); it_expoly != this->expolygons.end(); ++ it_expoly) {    
        // get unsupported bridge edges (both contour and holes)
        Lines unsupported_lines = to_lines(diff_pl(to_polylines(*it_expoly), grown_lower));
        /*  Split into individual segments and filter out edges parallel to the bridging angle
            TODO: angle tolerance should probably be based on segment length and flow width,
            so that we build supports whenever there's a chance that at least one or two bridge
            extrusions would be anchored within such length (i.e. a slightly non-parallel bridging
            direction might still benefit from anchors if long enough)
            double angle_tolerance = PI / 180.0 * 5.0; */
        for (const Line &line : unsupported_lines)
            if (! Slic3r::Geometry::directions_parallel(line.direction(), angle)) {
                unsupported->emplace_back(Polyline());
                unsupported->back().points.emplace_back(line.a);
                unsupported->back().points.emplace_back(line.b);
            }
    }
    
    /*
    if (0) {
        require "Slic3r/SVG.pm";
        Slic3r::SVG::output(
            "unsupported_" . rad2deg($angle) . ".svg",
            expolygons          => [$self->expolygon],
            green_expolygons    => $self->_anchor_regions,
            red_expolygons      => union_ex($grown_lower),
            no_arrows           => 1,
            polylines           => \@bridge_edges,
            red_polylines       => $unsupported,
        );
    }
    */
}

Polylines BridgeDetector::unsupported_edges(double angle) const {
    Polylines pp;
    this->unsupported_edges(angle, &pp);
    return pp;
}

}
