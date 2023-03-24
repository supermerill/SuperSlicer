#include <boost/log/trivial.hpp>

#include "MedialAxis.hpp"

#include "clipper.hpp"
#include "../ClipperUtils.hpp"

//#ifdef SLIC3R_DEBUG
//namespace boost { namespace polygon {
//
//// The following code for the visualization of the boost Voronoi diagram is based on:
////
//// Boost.Polygon library voronoi_graphic_utils.hpp header file
////          Copyright Andrii Sydorchuk 2010-2012.
//// Distributed under the Boost Software License, Version 1.0.
////    (See accompanying file LICENSE_1_0.txt or copy at
////          http://www.boost.org/LICENSE_1_0.txt)
//template <typename CT>
//class voronoi_visual_utils {
// public:
//  // Discretize parabolic Voronoi edge.
//  // Parabolic Voronoi edges are always formed by one point and one segment
//  // from the initial input set.
//  //
//  // Args:
//  //   point: input point.
//  //   segment: input segment.
//  //   max_dist: maximum discretization distance.
//  //   discretization: point discretization of the given Voronoi edge.
//  //
//  // Template arguments:
//  //   InCT: coordinate type of the input geometries (usually integer).
//  //   Point: point type, should model point concept.
//  //   Segment: segment type, should model segment concept.
//  //
//  // Important:
//  //   discretization should contain both edge endpoints initially.
//  template <class InCT1, class InCT2,
//            template<class> class Point,
//            template<class> class Segment>
//  static
//  typename enable_if<
//    typename gtl_and<
//      typename gtl_if<
//        typename is_point_concept<
//          typename geometry_concept< Point<InCT1> >::type
//        >::type
//      >::type,
//      typename gtl_if<
//        typename is_segment_concept<
//          typename geometry_concept< Segment<InCT2> >::type
//        >::type
//      >::type
//    >::type,
//    void
//  >::type discretize(
//      const Point<InCT1>& point,
//      const Segment<InCT2>& segment,
//      const CT max_dist,
//      std::vector< Point<CT> >* discretization) {
//    // Apply the linear transformation to move start point of the segment to
//    // the point with coordinates (0, 0) and the direction of the segment to
//    // coincide the positive direction of the x-axis.
//    CT segm_vec_x = cast(x(high(segment))) - cast(x(low(segment)));
//    CT segm_vec_y = cast(y(high(segment))) - cast(y(low(segment)));
//    CT sqr_segment_length = segm_vec_x * segm_vec_x + segm_vec_y * segm_vec_y;
//
//    // Compute x-coordinates of the endpoints of the edge
//    // in the transformed space.
//    CT projection_start = sqr_segment_length *
//        get_point_projection((*discretization)[0], segment);
//    CT projection_end = sqr_segment_length *
//        get_point_projection((*discretization)[1], segment);
//
//    // Compute parabola parameters in the transformed space.
//    // Parabola has next representation:
//    // f(x) = ((x-rot_x)^2 + rot_y^2) / (2.0*rot_y).
//    CT point_vec_x = cast(x(point)) - cast(x(low(segment)));
//    CT point_vec_y = cast(y(point)) - cast(y(low(segment)));
//    CT rot_x = segm_vec_x * point_vec_x + segm_vec_y * point_vec_y;
//    CT rot_y = segm_vec_x * point_vec_y - segm_vec_y * point_vec_x;
//
//    // Save the last point.
//    Point<CT> last_point = (*discretization)[1];
//    discretization->pop_back();
//
//    // Use stack to avoid recursion.
//    std::stack<CT> point_stack;
//    point_stack.push(projection_end);
//    CT cur_x = projection_start;
//    CT cur_y = parabola_y(cur_x, rot_x, rot_y);
//
//    // Adjust max_dist parameter in the transformed space.
//    const CT max_dist_transformed = max_dist * max_dist * sqr_segment_length;
//    while (!point_stack.empty()) {
//      CT new_x = point_stack.top();
//      CT new_y = parabola_y(new_x, rot_x, rot_y);
//
//      // Compute coordinates of the point of the parabola that is
//      // furthest from the current line segment.
//      CT mid_x = (new_y - cur_y) / (new_x - cur_x) * rot_y + rot_x;
//      CT mid_y = parabola_y(mid_x, rot_x, rot_y);
//
//      // Compute maximum distance between the given parabolic arc
//      // and line segment that discretize it.
//      CT dist = (new_y - cur_y) * (mid_x - cur_x) -
//          (new_x - cur_x) * (mid_y - cur_y);
//      dist = dist * dist / ((new_y - cur_y) * (new_y - cur_y) +
//          (new_x - cur_x) * (new_x - cur_x));
//      if (dist <= max_dist_transformed) {
//        // Distance between parabola and line segment is less than max_dist.
//        point_stack.pop();
//        CT inter_x = (segm_vec_x * new_x - segm_vec_y * new_y) /
//            sqr_segment_length + cast(x(low(segment)));
//        CT inter_y = (segm_vec_x * new_y + segm_vec_y * new_x) /
//            sqr_segment_length + cast(y(low(segment)));
//        discretization->push_back(Point<CT>(inter_x, inter_y));
//        cur_x = new_x;
//        cur_y = new_y;
//      } else {
//        point_stack.push(mid_x);
//      }
//    }
//
//    // Update last point.
//    discretization->back() = last_point;
//  }
//
// private:
//  // Compute y(x) = ((x - a) * (x - a) + b * b) / (2 * b).
//  static CT parabola_y(CT x, CT a, CT b) {
//    return ((x - a) * (x - a) + b * b) / (b + b);
//  }
//
//  // Get normalized length of the distance between:
//  //   1) point projection onto the segment
//  //   2) start point of the segment
//  // Return this length divided by the segment length. This is made to avoid
//  // sqrt computation during transformation from the initial space to the
//  // transformed one and vice versa. The assumption is made that projection of
//  // the point lies between the start-point and endpoint of the segment.
//  template <class InCT,
//            template<class> class Point,
//            template<class> class Segment>
//  static
//  typename enable_if<
//    typename gtl_and<
//      typename gtl_if<
//        typename is_point_concept<
//          typename geometry_concept< Point<int> >::type
//        >::type
//      >::type,
//      typename gtl_if<
//        typename is_segment_concept<
//          typename geometry_concept< Segment<long> >::type
//        >::type
//      >::type
//    >::type,
//    CT
//  >::type get_point_projection(
//      const Point<CT>& point, const Segment<InCT>& segment) {
//    CT segment_vec_x = cast(x(high(segment))) - cast(x(low(segment)));
//    CT segment_vec_y = cast(y(high(segment))) - cast(y(low(segment)));
//    CT point_vec_x = x(point) - cast(x(low(segment)));
//    CT point_vec_y = y(point) - cast(y(low(segment)));
//    CT sqr_segment_length =
//        segment_vec_x * segment_vec_x + segment_vec_y * segment_vec_y;
//    CT vec_dot = segment_vec_x * point_vec_x + segment_vec_y * point_vec_y;
//    return vec_dot / sqr_segment_length;
//  }
//
//  template <typename InCT>
//  static CT cast(const InCT& value) {
//    return static_cast<CT>(value);
//  }
//};
//
//} } // namespace boost::polygon
//#endif // SLIC3R_DEBUG

namespace Slic3r { namespace Geometry {


//#ifdef SLIC3R_DEBUG
//// The following code for the visualization of the boost Voronoi diagram is based on:
////
//// Boost.Polygon library voronoi_visualizer.cpp file
////          Copyright Andrii Sydorchuk 2010-2012.
//// Distributed under the Boost Software License, Version 1.0.
////    (See accompanying file LICENSE_1_0.txt or copy at
////          http://www.boost.org/LICENSE_1_0.txt)
//namespace Voronoi { namespace Internal {
//
//    typedef double coordinate_type;
//    typedef boost::polygon::point_data<coordinate_type> point_type;
//    typedef boost::polygon::segment_data<coordinate_type> segment_type;
//    typedef boost::polygon::rectangle_data<coordinate_type> rect_type;
//    typedef boost::polygon::voronoi_diagram<coordinate_type> VD;
//    typedef VD::cell_type cell_type;
//    typedef VD::cell_type::source_index_type source_index_type;
//    typedef VD::cell_type::source_category_type source_category_type;
//    typedef VD::edge_type edge_type;
//    typedef VD::cell_container_type cell_container_type;
//    typedef VD::cell_container_type vertex_container_type;
//    typedef VD::edge_container_type edge_container_type;
//    typedef VD::const_cell_iterator const_cell_iterator;
//    typedef VD::const_vertex_iterator const_vertex_iterator;
//    typedef VD::const_edge_iterator const_edge_iterator;
//
//    static const std::size_t EXTERNAL_COLOR = 1;
//
//    inline void color_exterior(const VD::edge_type* edge) 
//    {
//        if (edge->color() == EXTERNAL_COLOR)
//            return;
//        edge->color(EXTERNAL_COLOR);
//        edge->twin()->color(EXTERNAL_COLOR);
//        const VD::vertex_type* v = edge->vertex1();
//        if (v == NULL || !edge->is_primary())
//            return;
//        v->color(EXTERNAL_COLOR);
//        const VD::edge_type* e = v->incident_edge();
//        do {
//            color_exterior(e);
//            e = e->rot_next();
//        } while (e != v->incident_edge());
//    }
//
//    inline point_type retrieve_point(const std::vector<segment_type> &segments, const cell_type& cell) 
//    {
//        assert(cell.source_category() == boost::polygon::SOURCE_CATEGORY_SEGMENT_START_POINT || cell.source_category() == boost::polygon::SOURCE_CATEGORY_SEGMENT_END_POINT);
//        return (cell.source_category() == boost::polygon::SOURCE_CATEGORY_SEGMENT_START_POINT) ? low(segments[cell.source_index()]) : high(segments[cell.source_index()]);
//    }
//
//    inline void clip_infinite_edge(const std::vector<segment_type> &segments, const edge_type& edge, coordinate_type bbox_max_size, std::vector<point_type>* clipped_edge) 
//    {
//        const cell_type& cell1 = *edge.cell();
//        const cell_type& cell2 = *edge.twin()->cell();
//        point_type origin, direction;
//        // Infinite edges could not be created by two segment sites.
//        if (cell1.contains_point() && cell2.contains_point()) {
//            point_type p1 = retrieve_point(segments, cell1);
//            point_type p2 = retrieve_point(segments, cell2);
//            origin.x((p1.x() + p2.x()) * 0.5);
//            origin.y((p1.y() + p2.y()) * 0.5);
//            direction.x(p1.y() - p2.y());
//            direction.y(p2.x() - p1.x());
//        } else {
//            origin = cell1.contains_segment() ? retrieve_point(segments, cell2) : retrieve_point(segments, cell1);
//            segment_type segment = cell1.contains_segment() ? segments[cell1.source_index()] : segments[cell2.source_index()];
//            coordinate_type dx = high(segment).x() - low(segment).x();
//            coordinate_type dy = high(segment).y() - low(segment).y();
//            if ((low(segment) == origin) ^ cell1.contains_point()) {
//                direction.x(dy);
//                direction.y(-dx);
//            } else {
//                direction.x(-dy);
//                direction.y(dx);
//            }
//        }
//        coordinate_type koef = bbox_max_size / (std::max)(fabs(direction.x()), fabs(direction.y()));
//        if (edge.vertex0() == NULL) {
//            clipped_edge->push_back(point_type(
//                origin.x() - direction.x() * koef,
//                origin.y() - direction.y() * koef));
//        } else {
//            clipped_edge->push_back(
//                point_type(edge.vertex0()->x(), edge.vertex0()->y()));
//        }
//        if (edge.vertex1() == NULL) {
//            clipped_edge->push_back(point_type(
//                origin.x() + direction.x() * koef,
//                origin.y() + direction.y() * koef));
//        } else {
//            clipped_edge->push_back(
//                point_type(edge.vertex1()->x(), edge.vertex1()->y()));
//        }
//    }
//
//    inline void sample_curved_edge(const std::vector<segment_type> &segments, const edge_type& edge, std::vector<point_type> &sampled_edge, coordinate_type max_dist) 
//    {
//        point_type point = edge.cell()->contains_point() ?
//            retrieve_point(segments, *edge.cell()) :
//            retrieve_point(segments, *edge.twin()->cell());
//        segment_type segment = edge.cell()->contains_point() ?
//            segments[edge.twin()->cell()->source_index()] :
//            segments[edge.cell()->source_index()];
//        ::boost::polygon::voronoi_visual_utils<coordinate_type>::discretize(point, segment, max_dist, &sampled_edge);
//    }
//
//} /* namespace Internal */ } // namespace Voronoi
//
//void dump_voronoi_to_svg(const Lines &lines, /* const */ boost::polygon::voronoi_diagram<double> &vd, const ThickPolylines *polylines, const char *path)
//{
//    const double        scale                       = 0.2;
//    const std::string   inputSegmentPointColor      = "lightseagreen";
//    const coord_t       inputSegmentPointRadius     = coord_t(0.09 * scale / SCALING_FACTOR); 
//    const std::string   inputSegmentColor           = "lightseagreen";
//    const coord_t       inputSegmentLineWidth       = coord_t(0.03 * scale / SCALING_FACTOR);
//
//    const std::string   voronoiPointColor           = "black";
//    const coord_t       voronoiPointRadius          = coord_t(0.06 * scale / SCALING_FACTOR);
//    const std::string   voronoiLineColorPrimary     = "black";
//    const std::string   voronoiLineColorSecondary   = "green";
//    const std::string   voronoiArcColor             = "red";
//    const coord_t       voronoiLineWidth            = coord_t(0.02 * scale / SCALING_FACTOR);
//
//    const bool          internalEdgesOnly           = false;
//    const bool          primaryEdgesOnly            = false;
//
//    BoundingBox bbox = BoundingBox(lines);
//    bbox.min(0) -= coord_t(1. / SCALING_FACTOR);
//    bbox.min(1) -= coord_t(1. / SCALING_FACTOR);
//    bbox.max(0) += coord_t(1. / SCALING_FACTOR);
//    bbox.max(1) += coord_t(1. / SCALING_FACTOR);
//
//    ::Slic3r::SVG svg(path, bbox);
//
//    if (polylines != NULL)
//        svg.draw(*polylines, "lime", "lime", voronoiLineWidth);
//
////    bbox.scale(1.2);
//    // For clipping of half-lines to some reasonable value.
//    // The line will then be clipped by the SVG viewer anyway.
//    const double bbox_dim_max = double(bbox.max(0) - bbox.min(0)) + double(bbox.max(1) - bbox.min(1));
//    // For the discretization of the Voronoi parabolic segments.
//    const double discretization_step = 0.0005 * bbox_dim_max;
//
//    // Make a copy of the input segments with the double type.
//    std::vector<Voronoi::Internal::segment_type> segments;
//    for (Lines::const_iterator it = lines.begin(); it != lines.end(); ++ it)
//        segments.push_back(Voronoi::Internal::segment_type(
//            Voronoi::Internal::point_type(double(it->a(0)), double(it->a(1))), 
//            Voronoi::Internal::point_type(double(it->b(0)), double(it->b(1)))));
//    
//    // Color exterior edges.
//    for (boost::polygon::voronoi_diagram<double>::const_edge_iterator it = vd.edges().begin(); it != vd.edges().end(); ++it)
//        if (!it->is_finite())
//            Voronoi::Internal::color_exterior(&(*it));
//
//    // Draw the end points of the input polygon.
//    for (Lines::const_iterator it = lines.begin(); it != lines.end(); ++it) {
//        svg.draw(it->a, inputSegmentPointColor, inputSegmentPointRadius);
//        svg.draw(it->b, inputSegmentPointColor, inputSegmentPointRadius);
//    }
//    // Draw the input polygon.
//    for (Lines::const_iterator it = lines.begin(); it != lines.end(); ++it)
//        svg.draw(Line(Point(coord_t(it->a(0)), coord_t(it->a(1))), Point(coord_t(it->b(0)), coord_t(it->b(1)))), inputSegmentColor, inputSegmentLineWidth);
//
//#if 1
//    // Draw voronoi vertices.
//    for (boost::polygon::voronoi_diagram<double>::const_vertex_iterator it = vd.vertices().begin(); it != vd.vertices().end(); ++it)
//        if (! internalEdgesOnly || it->color() != Voronoi::Internal::EXTERNAL_COLOR)
//            svg.draw(Point(coord_t(it->x()), coord_t(it->y())), voronoiPointColor, voronoiPointRadius);
//
//    for (boost::polygon::voronoi_diagram<double>::const_edge_iterator it = vd.edges().begin(); it != vd.edges().end(); ++it) {
//        if (primaryEdgesOnly && !it->is_primary())
//            continue;
//        if (internalEdgesOnly && (it->color() == Voronoi::Internal::EXTERNAL_COLOR))
//            continue;
//        std::vector<Voronoi::Internal::point_type> samples;
//        std::string color = voronoiLineColorPrimary;
//        if (!it->is_finite()) {
//            Voronoi::Internal::clip_infinite_edge(segments, *it, bbox_dim_max, &samples);
//            if (! it->is_primary())
//                color = voronoiLineColorSecondary;
//        } else {
//            // Store both points of the segment into samples. sample_curved_edge will split the initial line
//            // until the discretization_step is reached.
//            samples.push_back(Voronoi::Internal::point_type(it->vertex0()->x(), it->vertex0()->y()));
//            samples.push_back(Voronoi::Internal::point_type(it->vertex1()->x(), it->vertex1()->y()));
//            if (it->is_curved()) {
//                Voronoi::Internal::sample_curved_edge(segments, *it, samples, discretization_step);
//                color = voronoiArcColor;
//            } else if (! it->is_primary())
//                color = voronoiLineColorSecondary;
//        }
//        for (std::size_t i = 0; i + 1 < samples.size(); ++i)
//            svg.draw(Line(Point(coord_t(samples[i].x()), coord_t(samples[i].y())), Point(coord_t(samples[i+1].x()), coord_t(samples[i+1].y()))), color, voronoiLineWidth);
//    }
//#endif
//
//    if (polylines != NULL)
//        svg.draw(*polylines, "blue", voronoiLineWidth);
//
//    svg.Close();
//}
//#endif // SLIC3R_DEBUG
//
//template<typename VD, typename SEGMENTS>
//inline const typename VD::point_type retrieve_cell_point(const typename VD::cell_type& cell, const SEGMENTS &segments)
//{
//    assert(cell.source_category() == boost::polygon::SOURCE_CATEGORY_SEGMENT_START_POINT || cell.source_category() == boost::polygon::SOURCE_CATEGORY_SEGMENT_END_POINT);
//    return (cell.source_category() == boost::polygon::SOURCE_CATEGORY_SEGMENT_START_POINT) ? low(segments[cell.source_index()]) : high(segments[cell.source_index()]);
//}
//
//template<typename VD, typename SEGMENTS>
//inline std::pair<typename VD::coord_type, typename VD::coord_type>
//measure_edge_thickness(const VD &vd, const typename VD::edge_type& edge, const SEGMENTS &segments)
//{
//    typedef typename VD::coord_type T;
//    const typename VD::point_type  pa(edge.vertex0()->x(), edge.vertex0()->y());
//    const typename VD::point_type  pb(edge.vertex1()->x(), edge.vertex1()->y());
//    const typename VD::cell_type  &cell1 = *edge.cell();
//    const typename VD::cell_type  &cell2 = *edge.twin()->cell();
//    if (cell1.contains_segment()) {
//        if (cell2.contains_segment()) {
//            // Both cells contain a linear segment, the left / right cells are symmetric.
//            // Project pa, pb to the left segment.
//            const typename VD::segment_type segment1 = segments[cell1.source_index()];
//            const typename VD::point_type p1a = project_point_to_segment(segment1, pa);
//            const typename VD::point_type p1b = project_point_to_segment(segment1, pb);
//            return std::pair<T, T>(T(2.)*dist(pa, p1a), T(2.)*dist(pb, p1b));
//        } else {
//            // 1st cell contains a linear segment, 2nd cell contains a point.
//            // The medial axis between the cells is a parabolic arc.
//            // Project pa, pb to the left segment.
//            const typename  VD::point_type p2 = retrieve_cell_point<VD>(cell2, segments);
//            return std::pair<T, T>(T(2.)*dist(pa, p2), T(2.)*dist(pb, p2));
//        }
//    } else if (cell2.contains_segment()) {
//        // 1st cell contains a point, 2nd cell contains a linear segment.
//        // The medial axis between the cells is a parabolic arc.
//        const typename VD::point_type p1 = retrieve_cell_point<VD>(cell1, segments);
//        return std::pair<T, T>(T(2.)*dist(pa, p1), T(2.)*dist(pb, p1));
//    } else {
//        // Both cells contain a point. The left / right regions are triangular and symmetric.
//        const typename VD::point_type p1 = retrieve_cell_point<VD>(cell1, segments);
//        return std::pair<T, T>(T(2.)*dist(pa, p1), T(2.)*dist(pb, p1));
//    }
//}
//
//// Converts the Line instances of Lines vector to VD::segment_type.
//template<typename VD>
//class Lines2VDSegments
//{
//public:
//    Lines2VDSegments(const Lines &alines) : lines(alines) {}
//    typename VD::segment_type operator[](size_t idx) const {
//        return typename VD::segment_type(
//            typename VD::point_type(typename VD::coord_type(lines[idx].a(0)), typename VD::coord_type(lines[idx].a(1))),
//            typename VD::point_type(typename VD::coord_type(lines[idx].b(0)), typename VD::coord_type(lines[idx].b(1))));
//    }
//private:
//    const Lines &lines;
//};
//
//void
//MedialAxis::build(ThickPolylines* polylines)
//{
//    construct_voronoi(this->lines.begin(), this->lines.end(), &this->vd);
//    
//    /*
//    // DEBUG: dump all Voronoi edges
//    {
//        for (VD::const_edge_iterator edge = this->vd.edges().begin(); edge != this->vd.edges().end(); ++edge) {
//            if (edge->is_infinite()) continue;
//            
//            ThickPolyline polyline;
//            polyline.points.push_back(Point( edge->vertex0()->x(), edge->vertex0()->y() ));
//            polyline.points.push_back(Point( edge->vertex1()->x(), edge->vertex1()->y() ));
//            polylines->push_back(polyline);
//        }
//        return;
//    }
//    */
//    
//    //typedef const VD::vertex_type vert_t;
//    typedef const VD::edge_type   edge_t;
//    
//    // collect valid edges (i.e. prune those not belonging to MAT)
//    // note: this keeps twins, so it inserts twice the number of the valid edges
//    this->valid_edges.clear();
//    {
//        std::set<const VD::edge_type*> seen_edges;
//        for (VD::const_edge_iterator edge = this->vd.edges().begin(); edge != this->vd.edges().end(); ++edge) {
//            // if we only process segments representing closed loops, none if the
//            // infinite edges (if any) would be part of our MAT anyway
//            if (edge->is_secondary() || edge->is_infinite()) continue;
//        
//            // don't re-validate twins
//            if (seen_edges.find(&*edge) != seen_edges.end()) continue;  // TODO: is this needed?
//            seen_edges.insert(&*edge);
//            seen_edges.insert(edge->twin());
//            
//            if (!this->validate_edge(&*edge)) continue;
//            this->valid_edges.insert(&*edge);
//            this->valid_edges.insert(edge->twin());
//        }
//    }
//    this->edges = this->valid_edges;
//    
//    // iterate through the valid edges to build polylines
//    while (!this->edges.empty()) {
//        const edge_t* edge = *this->edges.begin();
//        
//        // start a polyline
//        ThickPolyline polyline;
//        polyline.points.push_back(Point( edge->vertex0()->x(), edge->vertex0()->y() ));
//        polyline.points.push_back(Point( edge->vertex1()->x(), edge->vertex1()->y() ));
//        polyline.width.push_back(this->thickness[edge].first);
//        polyline.width.push_back(this->thickness[edge].second);
//        
//        // remove this edge and its twin from the available edges
//        (void)this->edges.erase(edge);
//        (void)this->edges.erase(edge->twin());
//        
//        // get next points
//        this->process_edge_neighbors(edge, &polyline);
//        
//        // get previous points
//        {
//            ThickPolyline rpolyline;
//            this->process_edge_neighbors(edge->twin(), &rpolyline);
//            polyline.points.insert(polyline.points.begin(), rpolyline.points.rbegin(), rpolyline.points.rend());
//            polyline.width.insert(polyline.width.begin(), rpolyline.width.rbegin(), rpolyline.width.rend());
//            polyline.endpoints.first = rpolyline.endpoints.second;
//        }
//        
//        assert(polyline.width.size() == polyline.points.size()*2 - 2);
//        
//        // prevent loop endpoints from being extended
//        if (polyline.first_point() == polyline.last_point()) {
//            polyline.endpoints.first = false;
//            polyline.endpoints.second = false;
//        }
//        
//        // append polyline to result
//        polylines->push_back(polyline);
//    }
//
//    #ifdef SLIC3R_DEBUG
//    {
//        static int iRun = 0;
//        dump_voronoi_to_svg(this->lines, this->vd, polylines, debug_out_path("MedialAxis-%d.svg", iRun ++).c_str());
//        printf("Thick lines: ");
//        for (ThickPolylines::const_iterator it = polylines->begin(); it != polylines->end(); ++ it) {
//            ThickLines lines = it->thicklines();
//            for (ThickLines::const_iterator it2 = lines.begin(); it2 != lines.end(); ++ it2) {
//                printf("%f,%f ", it2->a_width, it2->b_width);
//            }
//        }
//        printf("\n");
//    }
//    #endif /* SLIC3R_DEBUG */
//}
//
//void
//MedialAxis::build(Polylines* polylines)
//{
//    ThickPolylines tp;
//    this->build(&tp);
//    polylines->insert(polylines->end(), tp.begin(), tp.end());
//}
//
//void
//MedialAxis::process_edge_neighbors(const VD::edge_type* edge, ThickPolyline* polyline)
//{
//    while (true) {
//        // Since rot_next() works on the edge starting point but we want
//        // to find neighbors on the ending point, we just swap edge with
//        // its twin.
//        const VD::edge_type* twin = edge->twin();
//    
//        // count neighbors for this edge
//        std::vector<const VD::edge_type*> neighbors;
//        for (const VD::edge_type* neighbor = twin->rot_next(); neighbor != twin;
//            neighbor = neighbor->rot_next()) {
//            if (this->valid_edges.count(neighbor) > 0) neighbors.push_back(neighbor);
//        }
//    
//        // if we have a single neighbor then we can continue recursively
//        if (neighbors.size() == 1) {
//            const VD::edge_type* neighbor = neighbors.front();
//            
//            // break if this is a closed loop
//            if (this->edges.count(neighbor) == 0) return;
//            
//            Point new_point(neighbor->vertex1()->x(), neighbor->vertex1()->y());
//            polyline->points.push_back(new_point);
//            polyline->width.push_back(this->thickness[neighbor].first);
//            polyline->width.push_back(this->thickness[neighbor].second);
//            (void)this->edges.erase(neighbor);
//            (void)this->edges.erase(neighbor->twin());
//            edge = neighbor;
//        } else if (neighbors.size() == 0) {
//            polyline->endpoints.second = true;
//            return;
//        } else {
//            // T-shaped or star-shaped joint
//            return;
//        }
//    }
//}
//
//bool MedialAxis::validate_edge(const VD::edge_type* edge)
//{
//    // prevent overflows and detect almost-infinite edges
//#ifndef CLIPPERLIB_INT32
//    if (std::abs(edge->vertex0()->x()) > double(CLIPPER_MAX_COORD_UNSCALED) || 
//        std::abs(edge->vertex0()->y()) > double(CLIPPER_MAX_COORD_UNSCALED) || 
//        std::abs(edge->vertex1()->x()) > double(CLIPPER_MAX_COORD_UNSCALED) ||
//        std::abs(edge->vertex1()->y()) > double(CLIPPER_MAX_COORD_UNSCALED))
//        return false;
//#endif // CLIPPERLIB_INT32
//
//    // construct the line representing this edge of the Voronoi diagram
//    const Line line(
//        Point( edge->vertex0()->x(), edge->vertex0()->y() ),
//        Point( edge->vertex1()->x(), edge->vertex1()->y() )
//    );
//    
//    // discard edge if it lies outside the supplied shape
//    // this could maybe be optimized (checking inclusion of the endpoints
//    // might give false positives as they might belong to the contour itself)
//    if (this->m_expolygon != NULL) {
//        if (line.a == line.b) {
//            // in this case, contains(line) returns a false positive
//            if (!this->m_expolygon->contains(line.a)) return false;
//        } else {
//            if (!this->m_expolygon->contains(line)) return false;
//        }
//    }
//    
//    // retrieve the original line segments which generated the edge we're checking
//    const VD::cell_type* cell_l = edge->cell();
//    const VD::cell_type* cell_r = edge->twin()->cell();
//    const Line &segment_l = this->retrieve_segment(cell_l);
//    const Line &segment_r = this->retrieve_segment(cell_r);
//    
//    /*
//    SVG svg("edge.svg");
//    svg.draw(*this->m_expolygon);
//    svg.draw(line);
//    svg.draw(segment_l, "red");
//    svg.draw(segment_r, "blue");
//    svg.Close();
//    */
//    
//    /*  Calculate thickness of the cross-section at both the endpoints of this edge.
//        Our Voronoi edge is part of a CCW sequence going around its Voronoi cell 
//        located on the left side. (segment_l).
//        This edge's twin goes around segment_r. Thus, segment_r is 
//        oriented in the same direction as our main edge, and segment_l is oriented
//        in the same direction as our twin edge.
//        We used to only consider the (half-)distances to segment_r, and that works
//        whenever segment_l and segment_r are almost specular and facing. However, 
//        at curves they are staggered and they only face for a very little length
//        (our very short edge represents such visibility).
//        Both w0 and w1 can be calculated either towards cell_l or cell_r with equal
//        results by Voronoi definition.
//        When cell_l or cell_r don't refer to the segment but only to an endpoint, we
//        calculate the distance to that endpoint instead.  */
//    
//    coordf_t w0 = cell_r->contains_segment()
//        ? segment_r.distance_to(line.a)*2
//        : (this->retrieve_endpoint(cell_r) - line.a).cast<double>().norm()*2;
//    
//    coordf_t w1 = cell_l->contains_segment()
//        ? segment_l.distance_to(line.b)*2
//        : (this->retrieve_endpoint(cell_l) - line.b).cast<double>().norm()*2;
//    
//    if (cell_l->contains_segment() && cell_r->contains_segment()) {
//        // calculate the relative angle between the two boundary segments
//        double angle = fabs(segment_r.orientation() - segment_l.orientation());
//        if (angle > PI) angle = 2*PI - angle;
//        assert(angle >= 0 && angle <= PI);
//        
//        // fabs(angle) ranges from 0 (collinear, same direction) to PI (collinear, opposite direction)
//        // we're interested only in segments close to the second case (facing segments)
//        // so we allow some tolerance.
//        // this filter ensures that we're dealing with a narrow/oriented area (longer than thick)
//        // we don't run it on edges not generated by two segments (thus generated by one segment
//        // and the endpoint of another segment), since their orientation would not be meaningful
//        if (PI - angle > PI/8) {
//            // angle is not narrow enough
//            
//            // only apply this filter to segments that are not too short otherwise their 
//            // angle could possibly be not meaningful
//            if (w0 < SCALED_EPSILON || w1 < SCALED_EPSILON || line.length() >= this->m_min_width)
//                return false;
//        }
//    } else {
//        if (w0 < SCALED_EPSILON || w1 < SCALED_EPSILON)
//            return false;
//    }
//    
//    if (w0 < this->m_min_width && w1 < this->m_min_width)
//        return false;
//    
//    if (w0 > this->m_max_width && w1 > this->m_max_width)
//        return false;
//    
//    this->thickness[edge]         = std::make_pair(w0, w1);
//    this->thickness[edge->twin()] = std::make_pair(w1, w0);
//    
//    return true;
//}
//
//const Line& MedialAxis::retrieve_segment(const VD::cell_type* cell) const
//{
//    return this->lines[cell->source_index()];
//}
//
//const Point& MedialAxis::retrieve_endpoint(const VD::cell_type* cell) const
//{
//    const Line& line = this->retrieve_segment(cell);
//    if (cell->source_category() == boost::polygon::SOURCE_CATEGORY_SEGMENT_START_POINT) {
//        return line.a;
//    } else {
//        return line.b;
//    }
//}


//SUPERSLICER version


void
MedialAxis::build(Polylines& polylines)
{
    //TODO: special case for triangles
    //  take the longest edge
    //  take the opposite vertex and get the otho dist
    //  move the longest edge by X% that dist (depends on angle? from 1/2 to 1/4? or always 1/3?) use move dist as width
    //  clip it and then enlarge it into anchor
    //  note: ensure that if anchor is over only one edge, it's not the one choosen.

    //TODO: special case for quasi-rectangle
    //  take longest (not-anchor if any) edge
    //  get mid-dist for each adjascent edge
    //  use these point to get the line, with the mid-dist as widths.
    //  enlarge it into anchor

    ThickPolylines tp;
    this->build(tp);
    polylines.insert(polylines.end(), tp.begin(), tp.end());
}

void
MedialAxis::polyline_from_voronoi(const ExPolygon& voronoi_polygon, ThickPolylines* polylines)
{
    std::map<const VD::edge_type*, std::pair<coordf_t, coordf_t> > thickness;
    Lines lines = voronoi_polygon.lines();
    VD vd;
    ExPolygons poly_temp;
    const ExPolygon* poly_to_use = &voronoi_polygon;
    construct_voronoi(lines.begin(), lines.end(), &vd);
    //use a degraded mode, so it won't slow down too much #2664
     // first simplify from resolution, to see where we are
    if (vd.edges().size() > 20000) {
        poly_temp = poly_to_use->simplify(this->m_resolution / 2);
        if (poly_temp.size() == 1) poly_to_use = &poly_temp.front();
        lines = poly_to_use->lines();
        vd.clear();
        construct_voronoi(lines.begin(), lines.end(), &vd);
    }
    // maybe a second one, and this time, use an adapted resolution
    if (vd.edges().size() > 20000) {
        poly_temp = poly_to_use->simplify(this->m_resolution * (vd.edges().size() / 40000.));
        if (poly_temp.size() == 1) poly_to_use = &poly_temp.front();
        lines = poly_to_use->lines();
        vd.clear();
        construct_voronoi(lines.begin(), lines.end(), &vd);
    }

    typedef const VD::edge_type   edge_t;

    // DEBUG: dump all Voronoi edges
    //{
    //    std::stringstream stri;
    //    stri << "medial_axis_04_voronoi_" << this->id << ".svg";
    //    SVG svg(stri.str());
    //    for (VD::const_edge_iterator edge = vd.edges().begin(); edge != vd.edges().end(); ++edge) {
    //        if (edge->is_infinite()) continue;
    //        const edge_t* edgeptr = &*edge;
    //        ThickPolyline polyline;
    //        polyline.points.push_back(Point( edge->vertex0()->x(), edge->vertex0()->y() ));
    //        polyline.points.push_back(Point( edge->vertex1()->x(), edge->vertex1()->y() ));
    //        polyline.width.push_back(thickness[edgeptr].first);
    //        polyline.width.push_back(thickness[edgeptr].second);
    //        //polylines->push_back(polyline);
    //        svg.draw(polyline, "red");
    //    }
    //    svg.Close();
        //return;
    //}



    // collect valid edges (i.e. prune those not belonging to MAT)
    // note: this keeps twins, so it inserts twice the number of the valid edges
    std::set<const VD::edge_type*> valid_edges;
    {
        std::set<const edge_t*> seen_edges;
        for (VD::const_edge_iterator edge = vd.edges().begin(); edge != vd.edges().end(); ++edge) {
            // if we only process segments representing closed loops, none if the
            // infinite edges (if any) would be part of our MAT anyway
            if (edge->is_secondary() || edge->is_infinite()) continue;

            // don't re-validate twins
            if (seen_edges.find(&*edge) != seen_edges.end()) continue;  // TODO: is this needed?
            seen_edges.insert(&*edge);
            seen_edges.insert(edge->twin());

            if (!this->validate_edge(&*edge, lines, *poly_to_use, thickness)) continue;
            valid_edges.insert(&*edge);
            valid_edges.insert(edge->twin());
        }
    }
    std::set<const VD::edge_type*> edges = valid_edges;

    // iterate through the valid edges to build polylines
    while (!edges.empty()) {
        const edge_t* edge = *edges.begin();
        //if (thickness[edge].first > this->m_max_width * 1.001) {
            //std::cerr << "Error, edge.first has a thickness of " << unscaled(this->thickness[edge].first) << " > " << unscaled(this->max_width) << "\n";
            //(void)this->edges.erase(edge);
            //(void)this->edges.erase(edge->twin());
            //continue;
        //}
        //if (thickness[edge].second > this->m_max_width * 1.001) {
            //std::cerr << "Error, edge.second has a thickness of " << unscaled(this->thickness[edge].second) << " > " << unscaled(this->max_width) << "\n";
            //(void)this->edges.erase(edge);
            //(void)this->edges.erase(edge->twin());
            //continue;
        //}

        // start a polyline
        ThickPolyline polyline;
        polyline.points.push_back(Point(edge->vertex0()->x(), edge->vertex0()->y()));
        polyline.points.push_back(Point(edge->vertex1()->x(), edge->vertex1()->y()));
        polyline.points_width.push_back(thickness[edge].first);
        polyline.points_width.push_back(thickness[edge].second);

        // remove this edge and its twin from the available edges
        (void)edges.erase(edge);
        (void)edges.erase(edge->twin());

        // get next points
        this->process_edge_neighbors(edge, &polyline, edges, valid_edges, thickness);

        // get previous points
        {
            ThickPolyline rpolyline;
            this->process_edge_neighbors(edge->twin(), &rpolyline, edges, valid_edges, thickness);
            polyline.points.insert(polyline.points.begin(), rpolyline.points.rbegin(), rpolyline.points.rend());
            polyline.points_width.insert(polyline.points_width.begin(), rpolyline.points_width.rbegin(), rpolyline.points_width.rend());
            polyline.endpoints.first = rpolyline.endpoints.second;
        }

        assert(polyline.points_width.size() == polyline.points.size());

        // if loop, set endpoints to false
        // prevent loop endpoints from being extended
        if (polyline.first_point().coincides_with(polyline.last_point())) {
            polyline.endpoints.first = false;
            polyline.endpoints.second = false;
        }

        // append polyline to result
        polylines->push_back(polyline);
    }

#ifdef SLIC3R_DEBUG
    {
        static int iRun = 0;
        dump_voronoi_to_svg(this->lines, this->vd, polylines, debug_out_path("MedialAxis-%d.svg", iRun++).c_str());
        printf("Thick lines: ");
        for (ThickPolylines::const_iterator it = polylines->begin(); it != polylines->end(); ++it) {
            ThickLines lines = it->thicklines();
            for (ThickLines::const_iterator it2 = lines.begin(); it2 != lines.end(); ++it2) {
                printf("%f,%f ", it2->a_width, it2->b_width);
            }
        }
        printf("\n");
    }
#endif /* SLIC3R_DEBUG */
}

void
MedialAxis::process_edge_neighbors(const VD::edge_type* edge, ThickPolyline* polyline, std::set<const VD::edge_type*>& edges, std::set<const VD::edge_type*>& valid_edges, std::map<const VD::edge_type*, std::pair<coordf_t, coordf_t> >& thickness)
{
    while (true) {
        // Since rot_next() works on the edge starting point but we want
        // to find neighbors on the ending point, we just swap edge with
        // its twin.
        const VD::edge_type* twin = edge->twin();

        // count neighbors for this edge
        std::vector<const VD::edge_type*> neighbors;
        for (const VD::edge_type* neighbor = twin->rot_next(); neighbor != twin;
            neighbor = neighbor->rot_next()) {
            if (valid_edges.count(neighbor) > 0) neighbors.push_back(neighbor);
        }

        // if we have a single neighbor then we can continue recursively
        if (neighbors.size() == 1) {
            const VD::edge_type* neighbor = neighbors.front();

            // break if this is a closed loop
            if (edges.count(neighbor) == 0) return;

            Point new_point(neighbor->vertex1()->x(), neighbor->vertex1()->y());
            polyline->points.push_back(new_point);
            polyline->points_width.push_back(thickness[neighbor].second);

            (void)edges.erase(neighbor);
            (void)edges.erase(neighbor->twin());
            edge = neighbor;
        } else if (neighbors.size() == 0) {
            polyline->endpoints.second = true;
            return;
        } else {
            // T-shaped or star-shaped joint
            return;
        }
    }
}

bool
MedialAxis::validate_edge(const VD::edge_type* edge, Lines& lines, const ExPolygon& expolygon_touse, std::map<const VD::edge_type*, std::pair<coordf_t, coordf_t> >& thickness)
{
    // not relevant anymore... prusa has removed the (1 << 17) from clipper
    const double CLIPPER_MAX_COORD_UNSCALED = 0x3FFFFFFFFFFFFFFFLL / (1 << 17);
    // prevent overflows and detect almost-infinite edges
    if (std::abs(edge->vertex0()->x()) > double(CLIPPER_MAX_COORD_UNSCALED) ||
        std::abs(edge->vertex0()->y()) > double(CLIPPER_MAX_COORD_UNSCALED) ||
        std::abs(edge->vertex1()->x()) > double(CLIPPER_MAX_COORD_UNSCALED) ||
        std::abs(edge->vertex1()->y()) > double(CLIPPER_MAX_COORD_UNSCALED) ||
        std::isnan(edge->vertex0()->x()) ||
        std::isnan(edge->vertex0()->y()) ||
        std::isnan(edge->vertex1()->x()) ||
        std::isnan(edge->vertex1()->y()))
        return false;

    // construct the line representing this edge of the Voronoi diagram
    const Line line(
        Point(edge->vertex0()->x(), edge->vertex0()->y()),
        Point(edge->vertex1()->x(), edge->vertex1()->y())
    );

    // discard edge if it lies outside the supplied shape
    // this could maybe be optimized (checking inclusion of the endpoints
    // might give false positives as they might belong to the contour itself)
    if (line.a.coincides_with_epsilon(line.b)) {
        // in this case, contains(line) returns a false positive
        if (!expolygon_touse.contains(line.a)) return false;
    } else {
        //test if  (!expolygon_touse.contains(line))
        //this if isn't perfect (the middle of the line may still be out of the polygon)
        //but this edge-case shouldn't occur anyway, by the way the voronoi is built.
        if (!expolygon_touse.contains(line.a) || !expolygon_touse.contains(line.b)) { //this if reduced diff_pl from 25% to 18% cpu usage
            //this line can count for 25% of slicing time, if not enclosed in if
            // and still, some geometries can wreck havoc here #2664
            Polylines external_bits = diff_pl(Polylines{ Polyline{ line.a, line.b } }, expolygon_touse);
            if (!external_bits.empty()) {
                //check if the bits that are not inside are under epsilon length
                coordf_t max_length = 0;
                for (Polyline& poly : external_bits) {
                    max_length = std::max(max_length, poly.length());
                }
                if (max_length > SCALED_EPSILON)
                    return false;
            }
        }
    }

    // retrieve the original line segments which generated the edge we're checking
    const VD::cell_type* cell_l = edge->cell();
    const VD::cell_type* cell_r = edge->twin()->cell();
    const Line& segment_l = this->retrieve_segment(cell_l, lines);
    const Line& segment_r = this->retrieve_segment(cell_r, lines);


    //SVG svg("edge.svg");
    //svg.draw(expolygon_touse);
    //svg.draw(line);
    //svg.draw(segment_l, "red");
    //svg.draw(segment_r, "blue");
    //svg.Close();
    //

    /*  Calculate thickness of the cross-section at both the endpoints of this edge.
        Our Voronoi edge is part of a CCW sequence going around its Voronoi cell
        located on the left side. (segment_l).
        This edge's twin goes around segment_r. Thus, segment_r is
        oriented in the same direction as our main edge, and segment_l is oriented
        in the same direction as our twin edge.
        We used to only consider the (half-)distances to segment_r, and that works
        whenever segment_l and segment_r are almost specular and facing. However,
        at curves they are staggered and they only face for a very little length
        (our very short edge represents such visibility).
        Both w0 and w1 can be calculated either towards cell_l or cell_r with equal
        results by Voronoi definition.
        When cell_l or cell_r don't refer to the segment but only to an endpoint, we
        calculate the distance to that endpoint instead.  */

    coordf_t w0 = cell_r->contains_segment()
        ? line.a.distance_to(segment_r) * 2
        : line.a.distance_to(this->retrieve_endpoint(cell_r, lines)) * 2;

    coordf_t w1 = cell_l->contains_segment()
        ? line.b.distance_to(segment_l) * 2
        : line.b.distance_to(this->retrieve_endpoint(cell_l, lines)) * 2;

    //don't remove the line that goes to the intersection of the contour
    // we use them to create nicer thin wall lines
    //if (cell_l->contains_segment() && cell_r->contains_segment()) {
    //    // calculate the relative angle between the two boundary segments
    //    double angle = fabs(segment_r.orientation() - segment_l.orientation());
    //    if (angle > PI) angle = 2*PI - angle;
    //    assert(angle >= 0 && angle <= PI);
    //    
    //    // fabs(angle) ranges from 0 (collinear, same direction) to PI (collinear, opposite direction)
    //    // we're interested only in segments close to the second case (facing segments)
    //    // so we allow some tolerance.
    //    // this filter ensures that we're dealing with a narrow/oriented area (longer than thick)
    //    // we don't run it on edges not generated by two segments (thus generated by one segment
    //    // and the endpoint of another segment), since their orientation would not be meaningful
    //    if (PI - angle > PI/8) {
    //        // angle is not narrow enough
    //        
    //        // only apply this filter to segments that are not too short otherwise their 
    //        // angle could possibly be not meaningful
    //        if (w0 < SCALED_EPSILON || w1 < SCALED_EPSILON || line.length() >= this->m_min_width)
    //            return false;
    //    }
    //} else {
    //    if (w0 < SCALED_EPSILON || w1 < SCALED_EPSILON)
    //        return false;
    //}

    // don't do that before we try to fusion them
    //if (w0 < this->m_min_width && w1 < this->m_min_width)
    //    return false;
    //

    //shouldn't occur if perimeter_generator is well made. *1.05 for a little wiggle room
    if (w0 > this->m_max_width * 1.05 && w1 > this->m_max_width * 1.05)
        return false;

    thickness[edge] = std::make_pair(w0, w1);
    thickness[edge->twin()] = std::make_pair(w1, w0);

    return true;
}

const Line&
MedialAxis::retrieve_segment(const VD::cell_type* cell, Lines& lines) const
{
    return lines[cell->source_index()];
}

const Point&
MedialAxis::retrieve_endpoint(const VD::cell_type* cell, Lines& lines) const
{
    const Line& line = this->retrieve_segment(cell, lines);
    if (cell->source_category() == boost::polygon::SOURCE_CATEGORY_SEGMENT_START_POINT) {
        return line.a;
    } else {
        return line.b;
    }
}


/// remove point that are at SCALED_EPSILON * 2 distance.
void
remove_point_too_near(ThickPolyline* to_reduce)
{
    const coord_t smallest = (coord_t)SCALED_EPSILON * 2;
    size_t id = 1;
    while (id < to_reduce->points.size() - 1) {
        coord_t newdist = (coord_t)std::min(to_reduce->points[id].distance_to(to_reduce->points[id - 1])
            , to_reduce->points[id].distance_to(to_reduce->points[id + 1]));
        if (newdist < smallest) {
            to_reduce->points.erase(to_reduce->points.begin() + id);
            to_reduce->points_width.erase(to_reduce->points_width.begin() + id);
            newdist = (coord_t)to_reduce->points[id].distance_to(to_reduce->points[id - 1]);
            //if you removed a point, it check if the next one isn't too near from the previous one.
            // if not, it bypass it.
            if (newdist > smallest) {
                ++id;
            }
        }
        //go to next one
        else ++id;
    }
}

/// add points  from pattern to to_modify at the same % of the length
/// so not add if an other point is present at the correct position
void
add_point_same_percent(ThickPolyline* pattern, ThickPolyline* to_modify)
{
    const coordf_t to_modify_length = to_modify->length();
    const double percent_epsilon = SCALED_EPSILON / to_modify_length;
    const coordf_t pattern_length = pattern->length();

    double percent_length = 0;
    for (size_t idx_point = 1; idx_point < pattern->points.size() - 1; ++idx_point) {
        percent_length += pattern->points[idx_point - 1].distance_to(pattern->points[idx_point]) / pattern_length;
        //find position 
        size_t idx_other = 1;
        double percent_length_other_before = 0;
        double percent_length_other = 0;
        while (idx_other < to_modify->points.size()) {
            percent_length_other_before = percent_length_other;
            percent_length_other += to_modify->points[idx_other - 1].distance_to(to_modify->points[idx_other])
                / to_modify_length;
            if (percent_length_other > percent_length - percent_epsilon) {
                //if higher (we have gone over it)
                break;
            }
            ++idx_other;
        }
        if (percent_length_other > percent_length + percent_epsilon) {
            //insert a new point before the position
            double percent_dist = (percent_length - percent_length_other_before) / (percent_length_other - percent_length_other_before);
            coordf_t new_width = to_modify->points_width[idx_other - 1] * (1 - percent_dist);
            new_width += to_modify->points_width[idx_other] * (percent_dist);
            to_modify->points_width.insert(to_modify->points_width.begin() + idx_other, new_width);
            to_modify->points.insert(
                to_modify->points.begin() + idx_other,
                to_modify->points[idx_other - 1].interpolate(percent_dist, to_modify->points[idx_other]));
        }
    }
}

/// find the nearest angle in the contour (or 2 nearest if it's difficult to choose) 
/// return 1 for an angle of 90° and 0 for an angle of 0° or 180°
/// find the nearest angle in the contour (or 2 nearest if it's difficult to choose) 
/// return 1 for an angle of 90° and 0 for an angle of 0° or 180°
double
get_coeff_from_angle_countour(Point& point, const ExPolygon& contour, coord_t min_dist_between_point) {
    coordf_t nearest_dist = point.distance_to(contour.contour.points.front());
    Point point_nearest = contour.contour.points.front();
    size_t id_nearest = 0;
    coordf_t near_dist = nearest_dist;
    Point point_near = point_nearest;
    size_t id_near = 0;
    for (size_t id_point = 1; id_point < contour.contour.points.size(); ++id_point) {
        if (nearest_dist > point.distance_to(contour.contour.points[id_point])) {
            //update point_near
            id_near = id_nearest;
            point_near = point_nearest;
            near_dist = nearest_dist;
            //update nearest
            nearest_dist = point.distance_to(contour.contour.points[id_point]);
            point_nearest = contour.contour.points[id_point];
            id_nearest = id_point;
        }
    }
    double angle = 0;
    size_t id_before = id_nearest == 0 ? contour.contour.points.size() - 1 : id_nearest - 1;
    Point point_before = id_nearest == 0 ? contour.contour.points.back() : contour.contour.points[id_nearest - 1];
    //Search one point far enough to be relevant
    while (point_nearest.distance_to(point_before) < min_dist_between_point) {
        point_before = id_before == 0 ? contour.contour.points.back() : contour.contour.points[id_before - 1];
        id_before = id_before == 0 ? contour.contour.points.size() - 1 : id_before - 1;
        //don't loop
        if (id_before == id_nearest) {
            id_before = id_nearest == 0 ? contour.contour.points.size() - 1 : id_nearest - 1;
            point_before = id_nearest == 0 ? contour.contour.points.back() : contour.contour.points[id_nearest - 1];
            break;
        }
    }
    size_t id_after = id_nearest == contour.contour.points.size() - 1 ? 0 : id_nearest + 1;
    Point point_after = id_nearest == contour.contour.points.size() - 1 ? contour.contour.points.front() : contour.contour.points[id_nearest + 1];
    //Search one point far enough to be relevant
    while (point_nearest.distance_to(point_after) < min_dist_between_point) {
        point_after = id_after == contour.contour.points.size() - 1 ? contour.contour.points.front() : contour.contour.points[id_after + 1];
        id_after = id_after == contour.contour.points.size() - 1 ? 0 : id_after + 1;
        //don't loop
        if (id_after == id_nearest) {
            id_after = id_nearest == contour.contour.points.size() - 1 ? 0 : id_nearest + 1;
            point_after = id_nearest == contour.contour.points.size() - 1 ? contour.contour.points.front() : contour.contour.points[id_nearest + 1];
            break;
        }
    }
    //compute angle
    angle = point_nearest.ccw_angle(point_before, point_after);
    if (angle >= PI) angle = 2 * PI - angle;  // smaller angle
    //compute the diff from 90°
    angle = abs(angle - PI / 2);
    if (point_near.coincides_with_epsilon(point_nearest) && std::max(nearest_dist, near_dist) + SCALED_EPSILON < point_nearest.distance_to(point_near)) {
        //not only nearest
        Point point_before = id_near == 0 ? contour.contour.points.back() : contour.contour.points[id_near - 1];
        Point point_after = id_near == contour.contour.points.size() - 1 ? contour.contour.points.front() : contour.contour.points[id_near + 1];
        double angle2 = std::min(point_nearest.ccw_angle(point_before, point_after), point_nearest.ccw_angle(point_after, point_before));
        angle2 = abs(angle - PI / 2);
        angle = (angle + angle2) / 2;
    }

    return 1 - (angle / (PI / 2));
}

double
dot(Line l1, Line l2)
{
    Vec2d v_1(l1.b.x() - l1.a.x(), l1.b.y() - l1.a.y());
    v_1.normalize();
    Vec2d v_2(l2.b.x() - l2.a.x(), l2.b.y() - l2.a.y());
    v_2.normalize();
    return v_1.x() * v_2.x() + v_1.y() * v_2.y();
}

void
MedialAxis::fusion_curve(ThickPolylines& pp)
{
    //fusion Y with only 1 '0' value => the "0" branch "pull" the cross-point
    bool changes = false;
    for (size_t i = 0; i < pp.size(); ++i) {
        ThickPolyline& polyline = pp[i];
        // only consider 2-point polyline with endpoint
        //if (polyline.points.size() != 2) continue; // too restrictive.
        if (polyline.endpoints.first) polyline.reverse();
        else if (!polyline.endpoints.second) continue;
        if (polyline.points_width.back() > EPSILON) continue;

        //check my length is small
        coord_t length = (coord_t)polyline.length();
        if (length > this->m_max_width) continue;

        size_t closest_point_idx = this->m_expolygon.contour.closest_point_index(polyline.points.back());

        //check the 0-width point is on the contour.
        if (closest_point_idx == (size_t)-1) continue;

        size_t prev_idx = closest_point_idx == 0 ? this->m_expolygon.contour.points.size() - 1 : closest_point_idx - 1;
        size_t next_idx = closest_point_idx == this->m_expolygon.contour.points.size() - 1 ? 0 : closest_point_idx + 1;
        double mindot = 1;
        mindot = std::min(mindot, abs(dot(Line(polyline.points[polyline.points.size() - 1], polyline.points[polyline.points.size() - 2]),
            (Line(this->m_expolygon.contour.points[closest_point_idx], this->m_expolygon.contour.points[prev_idx])))));
        mindot = std::min(mindot, abs(dot(Line(polyline.points[polyline.points.size() - 1], polyline.points[polyline.points.size() - 2]),
            (Line(this->m_expolygon.contour.points[closest_point_idx], this->m_expolygon.contour.points[next_idx])))));

        //compute angle
        double coeff_contour_angle = this->m_expolygon.contour.points[closest_point_idx].ccw_angle(this->m_expolygon.contour.points[prev_idx], this->m_expolygon.contour.points[next_idx]);
        if (coeff_contour_angle >= PI) coeff_contour_angle = 2 * PI - coeff_contour_angle;  // smaller angle
        //compute the diff from 90°
        coeff_contour_angle = abs(coeff_contour_angle - PI / 2);


        // look if other end is a cross point with almost 90° angle
        double sum_dot = 0;
        double min_dot = 0;
        // look if other end is a cross point with multiple other branch
        std::vector<size_t> crosspoint;
        for (size_t j = 0; j < pp.size(); ++j) {
            if (j == i) continue;
            ThickPolyline& other = pp[j];
            if (polyline.first_point().coincides_with_epsilon(other.last_point())) {
                other.reverse();
                crosspoint.push_back(j);
                double dot_temp = dot(Line(polyline.points[0], polyline.points[1]), (Line(other.points[0], other.points[1])));
                min_dot = std::min(min_dot, abs(dot_temp));
                sum_dot += dot_temp;
            } else if (polyline.first_point().coincides_with_epsilon(other.first_point())) {
                crosspoint.push_back(j);
                double dot_temp = dot(Line(polyline.points[0], polyline.points[1]), (Line(other.points[0], other.points[1])));
                min_dot = std::min(min_dot, abs(dot_temp));
                sum_dot += dot_temp;
            }
        }
        sum_dot = abs(sum_dot);

        //only consider very shallow angle for contour
        if (mindot > 0.15 &&
            (1 - (coeff_contour_angle / (PI / 2))) > 0.2) continue;

        //check if it's a line that we can pull
        if (crosspoint.size() != 2) continue;
        if (sum_dot > 0.2) continue;
        if (min_dot > 0.5) continue;
        //don't remove useful bits. TODO: use the mindot to know by how much to multiply (1 when 90°, 1.42 when 45+, 1 when 0°)
        if (polyline.length() > polyline.points_width.front() * 1.42) continue;

        //don't pull, it distords the line if there are too many points.
        //// pull it a bit, depends on my size, the dot?, and the coeff at my 0-end (~14% for a square, almost 0 for a gentle curve)
        //coord_t length_pull = polyline.length();
        //length_pull *= 0.144 * get_coeff_from_angle_countour(polyline.points.back(), this->m_expolygon, std::min(m_min_width, polyline.length() / 2));

        ////compute dir
        //Vectorf pull_direction(polyline.points[1].x() - polyline.points[0].x(), polyline.points[1].y() - polyline.points[0].y());
        //pull_direction = normalize(pull_direction);
        //pull_direction.x() *= length_pull;
        //pull_direction.y() *= length_pull;

        ////pull the points
        //Point &p1 = pp[crosspoint[0]].points[0];
        //p1.x() = p1.x() + (coord_t)pull_direction.x();
        //p1.y() = p1.y() + (coord_t)pull_direction.y();

        //Point &p2 = pp[crosspoint[1]].points[0];
        //p2.x() = p2.x() + (coord_t)pull_direction.x();
        //p2.y() = p2.y() + (coord_t)pull_direction.y();

        //delete the now unused polyline
        pp.erase(pp.begin() + i);
        --i;
        changes = true;
    }
    if (changes) {
        concatThickPolylines(pp);
        ///reorder, in case of change
        std::sort(pp.begin(), pp.end(), [](const ThickPolyline& a, const ThickPolyline& b) { return a.length() < b.length(); });
        //have to redo it to remove multi-branch bits.
        fusion_curve(pp);
    }
}

void
MedialAxis::remove_bits(ThickPolylines& pp)
{

    //remove small bits that stick out of the path
    bool changes = false;
    for (size_t i = 0; i < pp.size(); ++i) {
        ThickPolyline& polyline = pp[i];
        // only consider polyline with 0-end
        if (polyline.endpoints.first) polyline.reverse();
        else if (!polyline.endpoints.second) continue;
        if (polyline.points_width.back() > 0) continue;

        //check my length is small
        coordf_t length = polyline.length();
        if (length > coordf_t(this->m_max_width) * 1.5) {
            continue;
        }

        // look if other end is a cross point with multiple other branch
        std::vector<size_t> crosspoint;
        for (size_t j = 0; j < pp.size(); ++j) {
            if (j == i) continue;
            ThickPolyline& other = pp[j];
            if (polyline.first_point().coincides_with_epsilon(other.last_point())) {
                other.reverse();
                crosspoint.push_back(j);
            } else if (polyline.first_point().coincides_with_epsilon(other.first_point())) {
                crosspoint.push_back(j);
            }
        }
        if (crosspoint.size() < 2) continue;

        //check if is smaller or the other ones are not endpoits
        int nb_better_than_me = 0;
        for (int i = 0; i < crosspoint.size(); i++) {
            if (!pp[crosspoint[0]].endpoints.second || length <= pp[crosspoint[0]].length())
                nb_better_than_me++;
        }
        if (nb_better_than_me < 2) continue;

        //check if the length of the polyline is small vs width of the other lines
        coord_t local_max_width = 0;
        for (int i = 0; i < crosspoint.size(); i++) {
            local_max_width = std::max(local_max_width, pp[crosspoint[i]].points_width[0]);
        }
        if (length > coordf_t(local_max_width + this->m_min_width))
            continue;

        //delete the now unused polyline
        pp.erase(pp.begin() + i);
        --i;
        changes = true;
    }
    if (changes) {
        concatThickPolylines(pp);
        ///reorder, in case of change
        std::sort(pp.begin(), pp.end(), [](const ThickPolyline& a, const ThickPolyline& b) { return a.length() < b.length(); });
    }

    //TODO: check if there is a U-turn (almost 180° direction change) : remove it.
}

void
MedialAxis::fusion_corners(ThickPolylines& pp)
{

    //fusion Y with only 1 '0' value => the "0" branch "pull" the cross-point
    bool changes = false;
    for (size_t i = 0; i < pp.size(); ++i) {
        ThickPolyline& polyline = pp[i];
        // only consider polyline with 0-end
        //if (polyline.points.size() != 2) continue; // maybe we should have something to merge X-point to 2-point if it's near enough.
        if (polyline.endpoints.first) polyline.reverse();
        else if (!polyline.endpoints.second) continue;
        if (polyline.points_width.back() > this->m_min_width) continue;

        //check my length is small
        coord_t length = (coord_t)polyline.length();
        if (length > this->m_max_width) continue;

        // look if other end is a cross point with multiple other branch
        std::vector<size_t> crosspoint;
        for (size_t j = 0; j < pp.size(); ++j) {
            if (j == i) continue;
            ThickPolyline& other = pp[j];
            if (polyline.first_point().coincides_with_epsilon(other.last_point())) {
                other.reverse();
                crosspoint.push_back(j);
            } else if (polyline.first_point().coincides_with_epsilon(other.first_point())) {
                crosspoint.push_back(j);
            }
        }
        //check if it's a line that we can pull
        if (crosspoint.size() != 2) continue;

        // check if i am at the external side of a curve
        double angle1 = polyline.points[0].ccw_angle(polyline.points[1], pp[crosspoint[0]].points[1]);
        if (angle1 >= PI) angle1 = 2 * PI - angle1;  // smaller angle
        double angle2 = polyline.points[0].ccw_angle(polyline.points[1], pp[crosspoint[1]].points[1]);
        if (angle2 >= PI) angle2 = 2 * PI - angle2;  // smaller angle
        if (angle1 + angle2 < PI) continue;

        //check if is smaller or the other ones are not endpoits
        if (pp[crosspoint[0]].endpoints.second && length > pp[crosspoint[0]].length()) continue;
        if (pp[crosspoint[1]].endpoints.second && length > pp[crosspoint[1]].length()) continue;

        if (polyline.points_width.back() > 0) {
            //FIXME: also pull (a bit less) points that are near to this one.
            // if true, pull it a bit, depends on my size, the dot?, and the coeff at my 0-end (~14% for a square, almost 0 for a gentle curve)
            coord_t length_pull = (coord_t)polyline.length();
            length_pull *= (coord_t)(0.144 * get_coeff_from_angle_countour(
                polyline.points.back(),
                this->m_expolygon,
                std::min(this->m_min_width, (coord_t)(polyline.length() / 2))));

            //compute dir
            Vec2d pull_direction(polyline.points[1].x() - polyline.points[0].x(), polyline.points[1].y() - polyline.points[0].y());
            pull_direction.normalize();
            pull_direction.x() *= length_pull;
            pull_direction.y() *= length_pull;

            //pull the points
            Point& p1 = pp[crosspoint[0]].points[0];
            p1.x() = p1.x() + (coord_t)pull_direction.x();
            p1.y() = p1.y() + (coord_t)pull_direction.y();

            Point& p2 = pp[crosspoint[1]].points[0];
            p2.x() = p2.x() + (coord_t)pull_direction.x();
            p2.y() = p2.y() + (coord_t)pull_direction.y();
        }

        //delete the now unused polyline
        pp.erase(pp.begin() + i);
        --i;
        changes = true;
    }
    if (changes) {
        concatThickPolylines(pp);
        ///reorder, in case of change
        std::sort(pp.begin(), pp.end(), [](const ThickPolyline& a, const ThickPolyline& b) { return a.length() < b.length(); });
    }
}

void
MedialAxis::extends_line_both_side(ThickPolylines& pp) {
    // opening : offset2-+
    const ExPolygons anchors = opening_ex(diff_ex(*this->m_bounds, this->m_expolygon), double(this->m_resolution));
    for (size_t i = 0; i < pp.size(); ++i) {
        ThickPolyline& polyline = pp[i];
        this->extends_line(polyline, anchors, this->m_min_width);
        if (!polyline.points.empty()) {
            polyline.reverse();
            this->extends_line(polyline, anchors, this->m_min_width);
        }
        if (polyline.points.empty()) {
            pp.erase(pp.begin() + i);
            --i;
        }
    }
}

void
MedialAxis::extends_line(ThickPolyline& polyline, const ExPolygons& anchors, const coord_t join_width)
{
    // extend initial and final segments of each polyline if they're actual endpoints
    // We assign new endpoints to temporary variables because in case of a single-line
    // polyline, after we extend the start point it will be caught by the intersection()
    // call, so we keep the inner point until we perform the second intersection() as well
    if (polyline.endpoints.second && !this->m_bounds->has_boundary_point(polyline.points.back())) {
        size_t first_idx = polyline.points.size() - 2;
        Line line(*(polyline.points.begin() + first_idx), polyline.points.back());
        while (line.length() < double(this->m_resolution) && first_idx > 0) {
            first_idx--;
            line.a = *(polyline.points.begin() + first_idx);
        }
        // prevent the line from touching on the other side, otherwise intersection() might return that solution
        if (polyline.points.size() == 2 && this->m_expolygon.contains(line.midpoint())) line.a = line.midpoint();

        line.extend_end((double)this->m_max_width);
        Point new_back;
        if (this->m_expolygon.contour.has_boundary_point(polyline.points.back())) {
            new_back = polyline.points.back();
        } else {
            bool finded = this->m_expolygon.contour.first_intersection(line, &new_back);
            //verify also for holes.
            Point new_back_temp;
            for (Polygon hole : this->m_expolygon.holes) {
                if (hole.first_intersection(line, &new_back_temp)) {
                    if (!finded || line.a.distance_to(new_back_temp) < line.a.distance_to(new_back)) {
                        finded = true;
                        new_back = new_back_temp;
                    }
                }
            }
            // safety check if no intersection
            if (!finded) {
                if (!this->m_expolygon.contains(line.b)) {
                    //it's outside!!!
                    //if (!this->m_expolygon.contains(line.a)) {
                    //    std::cout << "Error, a line is formed that start outside a polygon, end outside of it and don't cross it!\n";
                    //} else {
                    //    std::cout << "Error, a line is formed that start in a polygon, end outside of it and don't cross it!\n";
                    //}

                    //{
                    //    std::stringstream stri;
                    //    stri << "Error_" << (count_error++) << ".svg";
                    //    SVG svg(stri.str());
                    //    svg.draw(anchors);
                    //    svg.draw(this->m_expolygon);
                    //    svg.draw(line);
                    //    svg.draw(polyline);
                    //    svg.Close();
                    //}
                    //it's not possible to print that
                    polyline.points.clear();
                    polyline.points_width.clear();
                    return;
                }
                new_back = line.b;
            }
            polyline.points.push_back(new_back);
            polyline.points_width.push_back(polyline.points_width.back());
        }
        Point new_bound;
        bool finded = this->m_bounds->contour.first_intersection(line, &new_bound);
        //verify also for holes.
        Point new_bound_temp;
        for (Polygon hole : this->m_bounds->holes) {
            if (hole.first_intersection(line, &new_bound_temp)) {
                if (!finded || line.a.distance_to(new_bound_temp) < line.a.distance_to(new_bound)) {
                    finded = true;
                    new_bound = new_bound_temp;
                }
            }
        }
        // safety check if no intersection
        if (!finded) {
            if (line.b.coincides_with_epsilon(polyline.points.back()))
                return;
            //check if we don't over-shoot inside us
            bool is_in_anchor = false;
            for (const ExPolygon& a : anchors) {
                if (a.contains(line.b)) {
                    is_in_anchor = true;
                    break;
                }
            }
            if (!is_in_anchor) return;
            new_bound = line.b;
        }
        /* if (new_bound.coincides_with_epsilon(new_back)) {
             return;
         }*/
         // find anchor
        Point best_anchor;
        coordf_t shortest_dist = (coordf_t)this->m_max_width;
        for (const ExPolygon& a : anchors) {
            Point p_maybe_inside = a.contour.centroid();
            coordf_t test_dist = new_bound.distance_to(p_maybe_inside) + new_back.distance_to(p_maybe_inside);
            //if (test_dist < m_max_width / 2 && (test_dist < shortest_dist || shortest_dist < 0)) {
            double angle_test = new_back.ccw_angle(p_maybe_inside, line.a);
            if (angle_test > PI) angle_test = 2 * PI - angle_test;
            if (test_dist < (coordf_t)this->m_max_width && test_dist<shortest_dist && abs(angle_test) > PI / 2) {
                shortest_dist = test_dist;
                best_anchor = p_maybe_inside;
            }
        }
        if (best_anchor.x() != 0 && best_anchor.y() != 0) {
            Point p_obj = best_anchor + new_bound;
            p_obj.x() /= 2;
            p_obj.y() /= 2;
            Line l2 = Line(new_back, p_obj);
            l2.extend_end((coordf_t)this->m_max_width);
            (void)this->m_bounds->contour.first_intersection(l2, &new_bound);
        }
        if (new_bound.coincides_with_epsilon(new_back))
            return;
        polyline.points.push_back(new_bound);
        //polyline.width.push_back(join_width);
        //it thickens the line a bit too early, imo
        polyline.points_width.push_back(polyline.points_width.back());
    }
}

void
MedialAxis::extends_line_extra(ThickPolylines& pp) {
    // opening : offset2-+
    for (size_t i = 0; i < pp.size(); ++i) {
        ThickPolyline& polyline = pp[i];

        if (polyline.endpoints.first) {
            polyline.extend_start(this->m_extension_length);
        }
        if (polyline.endpoints.second) {
            polyline.extend_end(this->m_extension_length);
        }
    }
}



void
MedialAxis::main_fusion(ThickPolylines& pp)
{
    //int idf = 0;

    bool changes = true;
    std::map<Point, double> coeff_angle_cache;
    while (changes) {
        concatThickPolylines(pp);
        //reoder pp by length (ascending) It's really important to do that to avoid building the line from the width insteand of the length
        std::sort(pp.begin(), pp.end(), [](const ThickPolyline& a, const ThickPolyline& b) {
            bool ahas0 = a.points_width.front() == 0 || a.points_width.back() == 0;
            bool bhas0 = b.points_width.front() == 0 || b.points_width.back() == 0;
            if (ahas0 && !bhas0) return true;
            if (!ahas0 && bhas0) return false;
            return a.length() < b.length();
            });
        changes = false;
        for (size_t i = 0; i < pp.size(); ++i) {
            ThickPolyline& polyline = pp[i];

            //simple check to see if i can be fusionned
            if (!polyline.endpoints.first && !polyline.endpoints.second) continue;


            ThickPolyline* best_candidate = nullptr;
            float best_dot = -1;
            size_t best_idx = 0;
            double dot_poly_branch = 0;
            double dot_candidate_branch = 0;

            bool find_main_branch = false;
            size_t biggest_main_branch_id = 0;
            coord_t biggest_main_branch_length = 0;

            // find another polyline starting here
            for (size_t j = i + 1; j < pp.size(); ++j) {
                ThickPolyline& other = pp[j];
                if (polyline.last_point().coincides_with_epsilon(other.last_point())) {
                    polyline.reverse();
                    other.reverse();
                } else if (polyline.first_point().coincides_with_epsilon(other.last_point())) {
                    other.reverse();
                } else if (polyline.first_point().coincides_with_epsilon(other.first_point())) {
                } else if (polyline.last_point().coincides_with_epsilon(other.first_point())) {
                    polyline.reverse();
                } else {
                    continue;
                }
                //std::cout << " try : " << i << ":" << j << " : " << 
                //    (polyline.points.size() < 2 && other.points.size() < 2) <<
                //    (!polyline.endpoints.second || !other.endpoints.second) <<
                //    ((polyline.points.back().distance_to(other.points.back())
                //    + (polyline.width.back() + other.width.back()) / 4)
                //    > m_max_width*1.05) <<
                //    (abs(polyline.length() - other.length()) > m_max_width) << "\n";

                //// mergeable tests
                if (polyline.points.size() < 2 && other.points.size() < 2) continue;
                if (!polyline.endpoints.second || !other.endpoints.second) continue;
                // test if the new width will not be too big if a fusion occur
                //note that this isn't the real calcul. It's just to avoid merging lines too far apart.
                if (
                    ((polyline.points.back().distance_to(other.points.back())
                        + (polyline.points_width.back() + other.points_width.back()) / 4)
                > this->m_max_width * 1.05))
                    continue;
                // test if the lines are not too different in length.
                if (abs(polyline.length() - other.length()) > (coordf_t)this->m_max_width) continue;


                //test if we don't merge with something too different and without any relevance.
                double coeffSizePolyI = 1;
                if (polyline.points_width.back() == 0) {
                    coeffSizePolyI = 0.1 + 0.9 * get_coeff_from_angle_countour(polyline.points.back(), this->m_expolygon, std::min(this->m_min_width, (coord_t)(polyline.length() / 2)));
                }
                double coeffSizeOtherJ = 1;
                if (other.points_width.back() == 0) {
                    coeffSizeOtherJ = 0.1 + 0.9 * get_coeff_from_angle_countour(other.points.back(), this->m_expolygon, std::min(this->m_min_width, (coord_t)(polyline.length() / 2)));
                }
                //std::cout << " try2 : " << i << ":" << j << " : "
                //    << (abs(polyline.length()*coeffSizePolyI - other.length()*coeffSizeOtherJ) > m_max_width / 2)
                //    << (abs(polyline.length()*coeffSizePolyI - other.length()*coeffSizeOtherJ) > m_max_width)
                //    << "\n";
                if (abs(polyline.length() * coeffSizePolyI - other.length() * coeffSizeOtherJ) > (coordf_t)(this->m_max_width / 2)) continue;


                //compute angle to see if it's better than previous ones (straighter = better).
                //we need to add how strait we are from our main.
                float test_dot = (float)(dot(polyline.lines().front(), other.lines().front()));

                // Get the branch/line in wich we may merge, if possible
                // with that, we can decide what is important, and how we can merge that.
                // angle_poly - angle_candi =90° => one is useless
                // both angle are equal => both are useful with same strength
                // ex: Y => | both are useful to crete a nice line
                // ex2: TTTTT => -----  these 90° useless lines should be discarded
                find_main_branch = false;
                biggest_main_branch_id = 0;
                biggest_main_branch_length = 0;
                for (size_t k = 0; k < pp.size(); ++k) {
                    //std::cout << "try to find main : " << k << " ? " << i << " " << j << " ";
                    if (k == i || k == j) continue;
                    ThickPolyline& main = pp[k];
                    if (polyline.first_point().coincides_with_epsilon(main.last_point())) {
                        main.reverse();
                        if (!main.endpoints.second)
                            find_main_branch = true;
                        else if (biggest_main_branch_length < main.length()) {
                            biggest_main_branch_id = k;
                            biggest_main_branch_length = (coord_t)main.length();
                        }
                    } else if (polyline.first_point().coincides_with_epsilon(main.first_point())) {
                        if (!main.endpoints.second)
                            find_main_branch = true;
                        else if (biggest_main_branch_length < main.length()) {
                            biggest_main_branch_id = k;
                            biggest_main_branch_length = (coord_t)main.length();
                        }
                    }
                    if (find_main_branch) {
                        //use this variable to store the good index and break to compute it
                        biggest_main_branch_id = k;
                        break;
                    }
                }
                double dot_poly_branch_test = 0.707;
                double dot_candidate_branch_test = 0.707;
                if (!find_main_branch && biggest_main_branch_length == 0) {
                    // nothing -> it's impossible!
                    dot_poly_branch_test = 0.707;
                    dot_candidate_branch_test = 0.707;
                    //std::cout << "no main branch... impossible!!\n";
                } else if (!find_main_branch && (
                    (pp[biggest_main_branch_id].length() < polyline.length() && (polyline.points_width.back() != 0 || pp[biggest_main_branch_id].points_width.back() == 0))
                    || (pp[biggest_main_branch_id].length() < other.length() && (other.points_width.back() != 0 || pp[biggest_main_branch_id].points_width.back() == 0)))) {
                    //the main branch should have no endpoint or be bigger!
                    //here, it have an endpoint, and is not the biggest -> bad!
                    //std::cout << "he main branch should have no endpoint or be bigger! here, it have an endpoint, and is not the biggest -> bad!\n";
                    continue;
                } else {
                    //compute the dot (biggest_main_branch_id)
                    dot_poly_branch_test = -dot(Line(polyline.points[0], polyline.points[1]), Line(pp[biggest_main_branch_id].points[0], pp[biggest_main_branch_id].points[1]));
                    dot_candidate_branch_test = -dot(Line(other.points[0], other.points[1]), Line(pp[biggest_main_branch_id].points[0], pp[biggest_main_branch_id].points[1]));
                    if (dot_poly_branch_test < 0) dot_poly_branch_test = 0;
                    if (dot_candidate_branch_test < 0) dot_candidate_branch_test = 0;
                    if (pp[biggest_main_branch_id].points_width.back() > 0)
                        test_dot += 2 * (float)dot_poly_branch;
                    //std::cout << "compute dot "<< dot_poly_branch_test<<" & "<< dot_candidate_branch_test <<"\n";
                }
                //test if it's useful to merge or not
                //ie, don't merge  'T' but ok for 'Y', merge only lines of not disproportionate different length (ratio max: 4) (or they are both with 0-width end)
                if (dot_poly_branch_test < 0.1 || dot_candidate_branch_test < 0.1 ||
                    (
                        ((polyline.length() > other.length() ? polyline.length() / other.length() : other.length() / polyline.length()) > 4)
                        && !(polyline.points_width.back() == 0 && other.points_width.back() == 0)
                        )) {
                    //std::cout << "not useful to merge\n";
                    continue;
                }
                if (test_dot > best_dot) {
                    best_candidate = &other;
                    best_idx = j;
                    best_dot = test_dot;
                    dot_poly_branch = dot_poly_branch_test;
                    dot_candidate_branch = dot_candidate_branch_test;
                    //{
                    //    std::cout << "going to merge: b1=" << i << ", b2=" << best_idx << ", main=" << biggest_main_branch_id << "\n";
                    //    std::cout << "b1=" << polyline.points.front().x() << " : " << polyline.points.front().y() << " => " << polyline.points.back().x() << " : " << polyline.points.back().y() << "\n";
                    //    std::cout << "b2=" << other.points.front().x() << " : " << other.points.front().y() << " => " << other.points.back().x() << " : " << other.points.back().y() << "\n";
                    //    std::cout << "main=" << pp[biggest_main_branch_id].points.front().x() << " : " << pp[biggest_main_branch_id].points.front().y() << " => " << pp[biggest_main_branch_id].points.back().x() << " : " << pp[biggest_main_branch_id].points.back().y() << "\n";
                    //}
                }
            }
            if (best_candidate != nullptr) {
                //idf++;
                //std::cout << " == fusion " << id <<" : "<< idf << " == with "<< i <<" & "<<best_idx<<"\n";
                // delete very near points
                remove_point_too_near(&polyline);
                remove_point_too_near(best_candidate);

                // add point at the same pos than the other line to have a nicer fusion
                add_point_same_percent(&polyline, best_candidate);
                add_point_same_percent(best_candidate, &polyline);

                //get the angle of the nearest points of the contour to see : _| (good) \_ (average) __(bad)
                //sqrt because the result are nicer this way: don't over-penalize /_ angles
                //TODO: try if we can achieve a better result if we use a different algo if the angle is <90°
                const double coeff_angle_poly = (coeff_angle_cache.find(polyline.points.back()) != coeff_angle_cache.end())
                    ? coeff_angle_cache[polyline.points.back()]
                    : (get_coeff_from_angle_countour(polyline.points.back(), this->m_expolygon, std::min(this->m_min_width, (coord_t)(polyline.length() / 2))));
                const double coeff_angle_candi = (coeff_angle_cache.find(best_candidate->points.back()) != coeff_angle_cache.end())
                    ? coeff_angle_cache[best_candidate->points.back()]
                    : (get_coeff_from_angle_countour(best_candidate->points.back(), this->m_expolygon, std::min(this->m_min_width, (coord_t)(best_candidate->length() / 2))));

                //this will encourage to follow the curve, a little, because it's shorter near the center
                //without that, it tends to go to the outter rim.
                //std::cout << " std::max(polyline.length(), best_candidate->length())=" << std::max(polyline.length(), best_candidate->length())
                //    << ", polyline.length()=" << polyline.length()
                //    << ", best_candidate->length()=" << best_candidate->length()
                //    << ", polyline.length() / max=" << (polyline.length() / std::max(polyline.length(), best_candidate->length()))
                //    << ", best_candidate->length() / max=" << (best_candidate->length() / std::max(polyline.length(), best_candidate->length()))
                //    << "\n";
                double weight_poly = 2 - (polyline.length() / std::max(polyline.length(), best_candidate->length()));
                double weight_candi = 2 - (best_candidate->length() / std::max(polyline.length(), best_candidate->length()));
                weight_poly *= coeff_angle_poly;
                weight_candi *= coeff_angle_candi;
                const double coeff_poly = (dot_poly_branch * weight_poly) / (dot_poly_branch * weight_poly + dot_candidate_branch * weight_candi);
                const double coeff_candi = 1.0 - coeff_poly;
                //std::cout << "coeff_angle_poly=" << coeff_angle_poly
                //    << ", coeff_angle_candi=" << coeff_angle_candi
                //    << ", weight_poly=" << (2 - (polyline.length() / std::max(polyline.length(), best_candidate->length())))
                //    << ", weight_candi=" << (2 - (best_candidate->length() / std::max(polyline.length(), best_candidate->length())))
                //    << ", sumpoly=" << weight_poly
                //    << ", sumcandi=" << weight_candi
                //    << ", dot_poly_branch=" << dot_poly_branch
                //    << ", dot_candidate_branch=" << dot_candidate_branch
                //    << ", coeff_poly=" << coeff_poly
                //    << ", coeff_candi=" << coeff_candi
                //    << "\n";
                //iterate the points
                // as voronoi should create symetric thing, we can iterate synchonously
                size_t idx_point = 1;
                while (idx_point < std::min(polyline.points.size(), best_candidate->points.size())) {
                    //fusion
                    polyline.points[idx_point].x() = (coord_t)(polyline.points[idx_point].x() * coeff_poly + best_candidate->points[idx_point].x() * coeff_candi);
                    polyline.points[idx_point].y() = (coord_t)(polyline.points[idx_point].y() * coeff_poly + best_candidate->points[idx_point].y() * coeff_candi);

                    // The width decrease with distance from the centerline.
                    // This formula is what works the best, even if it's not perfect (created empirically).  0->3% error on a gap fill on some tests.
                    //If someone find  an other formula based on the properties of the voronoi algorithm used here, and it works better, please use it.
                    //or maybe just use the distance to nearest edge in bounds...
                    double value_from_current_width = 0.5 * polyline.points_width[idx_point] * dot_poly_branch / std::max(dot_poly_branch, dot_candidate_branch);
                    value_from_current_width += 0.5 * best_candidate->points_width[idx_point] * dot_candidate_branch / std::max(dot_poly_branch, dot_candidate_branch);
                    double value_from_dist = 2 * polyline.points[idx_point].distance_to(best_candidate->points[idx_point]);
                    value_from_dist *= sqrt(std::min(dot_poly_branch, dot_candidate_branch) / std::max(dot_poly_branch, dot_candidate_branch));
                    polyline.points_width[idx_point] = value_from_current_width + value_from_dist;
                    //std::cout << "width:" << polyline.width[idx_point] << " = " << value_from_current_width << " + " << value_from_dist 
                    //    << " (<" << m_max_width << " && " << (this->m_bounds.contour.closest_point(polyline.points[idx_point])->distance_to(polyline.points[idx_point]) * 2.1)<<")\n";
                    //failsafes
                    if (polyline.points_width[idx_point] > this->m_max_width)
                        polyline.points_width[idx_point] = this->m_max_width;
                    //failsafe: try to not go out of the radius of the section, take the width of the merging point for that. (and with some offset)
                    coord_t main_branch_width = pp[biggest_main_branch_id].points_width.front();
                    coordf_t main_branch_dist = pp[biggest_main_branch_id].points.front().distance_to(polyline.points[idx_point]);
                    coord_t max_width_from_main = (coord_t)std::sqrt(main_branch_width * main_branch_width + main_branch_dist * main_branch_dist);
                    if (find_main_branch && polyline.points_width[idx_point] > max_width_from_main)
                        polyline.points_width[idx_point] = max_width_from_main;
                    if (find_main_branch && polyline.points_width[idx_point] > pp[biggest_main_branch_id].points_width.front() * 1.1)
                        polyline.points_width[idx_point] = coord_t(pp[biggest_main_branch_id].points_width.front() * 1.1);
                    //std::cout << "main fusion, max dist : " << max_width_from_main << "\n";

                    ++idx_point;
                }
                if (idx_point < best_candidate->points.size()) {
                    if (idx_point + 1 < best_candidate->points.size()) {
                        //create a new polyline
                        pp.emplace_back();
                        best_candidate = &pp[best_idx]; // have to refresh the pointer, as the emplace_back() may have moved the array
                        pp.back().endpoints.first = true;
                        pp.back().endpoints.second = best_candidate->endpoints.second;
                        for (size_t idx_point_new_line = idx_point; idx_point_new_line < best_candidate->points.size(); ++idx_point_new_line) {
                            pp.back().points.push_back(best_candidate->points[idx_point_new_line]);
                            pp.back().points_width.push_back(best_candidate->points_width[idx_point_new_line]);
                        }
                    } else {
                        //Add last point
                        polyline.points.push_back(best_candidate->points[idx_point]);
                        polyline.points_width.push_back(best_candidate->points_width[idx_point]);
                        //select if an end occur
                        polyline.endpoints.second &= best_candidate->endpoints.second;
                    }

                } else {
                    //select if an end occur
                    polyline.endpoints.second &= best_candidate->endpoints.second;
                }

                //remove points that are the same or too close each other, ie simplify
                for (size_t idx_point = 1; idx_point < polyline.points.size(); ++idx_point) {
                    if (polyline.points[idx_point - 1].distance_to(polyline.points[idx_point]) < SCALED_EPSILON) {
                        if (idx_point < polyline.points.size() - 1) {
                            polyline.points.erase(polyline.points.begin() + idx_point);
                            polyline.points_width.erase(polyline.points_width.begin() + idx_point);
                        } else {
                            polyline.points.erase(polyline.points.begin() + idx_point - 1);
                            polyline.points_width.erase(polyline.points_width.begin() + idx_point - 1);
                        }
                        --idx_point;
                    }
                }
                //remove points that are outside of the geometry
                for (size_t idx_point = 0; idx_point < polyline.points.size(); ++idx_point) {
                    if (!this->m_bounds->contains_b(polyline.points[idx_point])) {
                        polyline.points.erase(polyline.points.begin() + idx_point);
                        polyline.points_width.erase(polyline.points_width.begin() + idx_point);
                        --idx_point;
                    }
                }

                if (polyline.points.size() < 2) {
                    //remove self
                    pp.erase(pp.begin() + i);
                    --i;
                    --best_idx;
                } else {
                    //update cache
                    coeff_angle_cache[polyline.points.back()] = coeff_angle_poly * coeff_poly + coeff_angle_candi * coeff_candi;
                }

                pp.erase(pp.begin() + best_idx);
                //{
                //    std::stringstream stri;
                //    stri << "medial_axis_2.0_aft_fus_" << id << "_" << idf << ".svg";
                //    SVG svg(stri.str());
                //    svg.draw(this->m_bounds);
                //    svg.draw(this->m_expolygon);
                //    svg.draw(pp);
                //    svg.Close();
                //}
                changes = true;
                break;
            }
        }
    }
}

void
MedialAxis::remove_too_thin_extrusion(ThickPolylines& pp)
{
    // remove too thin extrusion at start & end of polylines
    bool changes = false;
    for (size_t i = 0; i < pp.size(); ++i) {
        ThickPolyline& polyline = pp[i];
        bool polyline_changes = false;
        // remove bits with too small extrusion
        while (polyline.points.size() > 1 && polyline.points_width.front() < this->m_min_width && polyline.endpoints.first) {
            //try to split if possible
            if (polyline.points_width[1] > this->m_min_width) {
                double percent_can_keep = (this->m_min_width - polyline.points_width[0]) / (polyline.points_width[1] - polyline.points_width[0]);
                if (polyline.points.front().distance_to(polyline.points[1]) * (1 - percent_can_keep) > coordf_t(this->m_resolution)) {
                    //Can split => move the first point and assign a new weight.
                    //the update of endpoints wil be performed in concatThickPolylines
                    polyline.points.front() = polyline.points.front().interpolate(percent_can_keep, polyline.points[1]);
                    polyline.points_width.front() = this->m_min_width;
                } else {
                    /// almost 0-length, Remove
                    polyline.points.erase(polyline.points.begin());
                    polyline.points_width.erase(polyline.points_width.begin());
                }
                changes = true;
                polyline_changes = true;
                break;
            }
            polyline.points.erase(polyline.points.begin());
            polyline.points_width.erase(polyline.points_width.begin());
            changes = true;
            polyline_changes = true;
        }
        while (polyline.points.size() > 1 && polyline.points_width.back() < this->m_min_width && polyline.endpoints.second) {
            //try to split if possible
            if (polyline.points_width[polyline.points.size() - 2] > this->m_min_width) {
                double percent_can_keep = (this->m_min_width - polyline.points_width.back()) / (polyline.points_width[polyline.points.size() - 2] - polyline.points_width.back());
                if (polyline.points.back().distance_to(polyline.points[polyline.points.size() - 2]) * (1 - percent_can_keep) > coordf_t(this->m_resolution)) {
                    //Can split => move the first point and assign a new weight.
                    //the update of endpoints wil be performed in concatThickPolylines
                    polyline.points.back() = polyline.points.back().interpolate(percent_can_keep, polyline.points[polyline.points.size() - 2]);
                    polyline.points_width.back() = this->m_min_width;
                } else {
                    /// almost 0-length, Remove
                    polyline.points.erase(polyline.points.end() - 1);
                    polyline.points_width.erase(polyline.points_width.end() - 1);
                }
                polyline_changes = true;
                changes = true;
                break;
            }
            polyline.points.erase(polyline.points.end() - 1);
            polyline.points_width.erase(polyline.points_width.end() - 1);
            polyline_changes = true;
            changes = true;
        }
        //remove points and bits that comes from a "main line"
        if (polyline.points.size() < 2 || (polyline_changes && polyline.points.size() == 2 && polyline.length() < std::max(this->m_min_length, std::max(polyline.points_width.front(), polyline.points_width.back()))) ) {
            //remove self if too small
            pp.erase(pp.begin() + i);
            --i;
        }
    }
    if (changes) concatThickPolylines(pp);
}

void
MedialAxis::remove_too_thick_extrusion(ThickPolylines& pp)
{
    if (this->m_biggest_width <= 0) return;
    // remove too thin extrusion at start & end of polylines
    bool changes = false;
    for (size_t i = 0; i < pp.size(); ++i) {
        ThickPolyline& polyline = pp[i];
        bool polyline_changes = false;
        // remove bits with too small extrusion
        while (polyline.points.size() > 1 && polyline.points_width.front() > this->m_biggest_width && polyline.endpoints.first) {
            //try to split if possible
            if (polyline.points_width[1] < this->m_biggest_width) {
                double percent_can_keep = (this->m_biggest_width - polyline.points_width[0]) / (polyline.points_width[1] - polyline.points_width[0]);
                if (polyline.points.front().distance_to(polyline.points[1]) * (1 - percent_can_keep) > coordf_t(this->m_resolution)) {
                    //Can split => move the first point and assign a new weight.
                    //the update of endpoints wil be performed in concatThickPolylines
                    polyline.points.front() = polyline.points.front().interpolate(percent_can_keep, polyline.points[1]);
                    polyline.points_width.front() = this->m_biggest_width;
                } else {
                    /// almost 0-length, Remove
                    polyline.points.erase(polyline.points.begin());
                    polyline.points_width.erase(polyline.points_width.begin());
                }
                changes = true;
                polyline_changes = true;
                break;
            }
            polyline.points.erase(polyline.points.begin());
            polyline.points_width.erase(polyline.points_width.begin());
            changes = true;
            polyline_changes = true;
        }
        while (polyline.points.size() > 1 && polyline.points_width.back() > this->m_biggest_width && polyline.endpoints.second) {
            //try to split if possible
            if (polyline.points_width[polyline.points.size() - 2] < this->m_biggest_width) {
                double percent_can_keep = (this->m_biggest_width - polyline.points_width.back()) / (polyline.points_width[polyline.points.size() - 2] - polyline.points_width.back());
                if (polyline.points.back().distance_to(polyline.points[polyline.points.size() - 2]) * (1 - percent_can_keep) > coordf_t(this->m_resolution)) {
                    //Can split => move the first point and assign a new weight.
                    //the update of endpoints wil be performed in concatThickPolylines
                    polyline.points.back() = polyline.points.back().interpolate(percent_can_keep, polyline.points[polyline.points.size() - 2]);
                    polyline.points_width.back() = this->m_biggest_width;
                } else {
                    /// almost 0-length, Remove
                    polyline.points.erase(polyline.points.end() - 1);
                    polyline.points_width.erase(polyline.points_width.end() - 1);
                }
                polyline_changes = true;
                changes = true;
                break;
            }
            polyline.points.erase(polyline.points.end() - 1);
            polyline.points_width.erase(polyline.points_width.end() - 1);
            polyline_changes = true;
            changes = true;
        }
        //remove points and bits that comes from a "main line"
        if (polyline.points.size() < 2 || (polyline_changes && polyline.points.size() == 2 && polyline.length() < std::max(this->m_min_length, std::max(polyline.points_width.front(), polyline.points_width.back())))) {
            //remove self if too small
            pp.erase(pp.begin() + i);
            --i;
        }
    }
    if (changes) concatThickPolylines(pp);
}

void
MedialAxis::concatenate_small_polylines(ThickPolylines& pp)
{
    /*
     new goal: ensure that if there is a too short segment, it will be connected with a sufficiently long one, to save it
    */
    const coordf_t shortest_size = (coordf_t)this->m_min_length;
    std::set<size_t> deleted;
    std::vector<size_t> idx_per_size;
    //TODO: cache the length
    for (size_t i = 0; i < pp.size(); ++i) {
        ThickPolyline& polyline = pp[i];
        if (polyline.endpoints.first && polyline.endpoints.second) continue; // optimization
        if(polyline.length() <= shortest_size)
            idx_per_size.push_back(i);
    }
    std::sort(idx_per_size.begin(), idx_per_size.end(), [&pp](size_t a, size_t b) -> bool {return pp[a].length() > pp[b].length(); });
    for (size_t idx_sorted = 0; idx_sorted < idx_per_size.size(); ++idx_sorted) {
        if (deleted.find(idx_per_size[idx_sorted]) != deleted.end()) continue;
        //all these polylines need to be saved
        ThickPolyline& polyline = pp[idx_per_size[idx_sorted]];

        ThickPolyline* best_candidate = nullptr;
        float best_dot = -1;
        coordf_t best_length = -1;
        size_t best_idx = 0;

        // find another polyline starting here
        for (size_t j = 0; j < pp.size(); ++j) {
            if (deleted.find(j) != deleted.end()) continue;
            if (j == idx_per_size[idx_sorted]) continue;
            ThickPolyline& other = pp[j];
            if (other.endpoints.first && other.endpoints.second) continue;
            coordf_t other_length = other.length();
            if (other_length + polyline.length() <= shortest_size) continue; // need to be long enough to save it
            bool me_reverse = false;
            bool other_reverse = false;
            if (polyline.last_point().coincides_with_epsilon(other.last_point())) {
                other_reverse = true;
            } else if (polyline.first_point().coincides_with_epsilon(other.last_point())) {
                me_reverse = true;
                other_reverse = true;
            } else if (polyline.first_point().coincides_with_epsilon(other.first_point())) {
                me_reverse = true;
            } else if (!polyline.last_point().coincides_with_epsilon(other.first_point())) {
                continue;
            }

            //find the straitest
            Vec2d v_poly(me_reverse ? polyline.lines().front().vector().x() : polyline.lines().back().vector().x(),
                me_reverse ? polyline.lines().front().vector().y() : polyline.lines().back().vector().y());
            v_poly *= (1 / std::sqrt(v_poly.x() * v_poly.x() + v_poly.y() * v_poly.y()));
            Vec2d v_other(other_reverse ? other.lines().back().vector().x() : other.lines().front().vector().x(),
                other_reverse ? other.lines().back().vector().y() : other.lines().front().vector().y());
            v_other *= (1 / std::sqrt(v_other.x() * v_other.x() + v_other.y() * v_other.y()));
            float other_dot = std::abs(float(v_poly.x() * v_other.x() + v_poly.y() * v_other.y()));
            // use the straitest one
            // but if almost equal, use the shortest one
            if (std::abs(other_dot - best_dot) < 0.01) {
                if (best_length < 0 || best_length > other_length) {
                    best_candidate = &other;
                    best_idx = j;
                    best_dot = other_dot;
                    best_length = other_length;
                }
            }else if (other_dot > best_dot) {
                best_candidate = &other;
                best_idx = j;
                best_dot = other_dot;
                best_length = other_length;
            }
        }
        if (best_candidate != nullptr && best_candidate->points.size() > 1) {
            if (polyline.last_point().coincides_with_epsilon(best_candidate->last_point())) {
                best_candidate->reverse();
            } else if (polyline.first_point().coincides_with_epsilon(best_candidate->last_point())) {
                polyline.reverse();
                best_candidate->reverse();
            } else if (polyline.first_point().coincides_with_epsilon(best_candidate->first_point())) {
                polyline.reverse();
            }
            //intersections may create over-extrusion because the included circle can be a bit larger. We have to make it short again if needed.
            if (polyline.points.size() > 1 && best_candidate->points.size() > 1
                && polyline.points_width.back() > polyline.points_width[polyline.points_width.size() - 2]
                && polyline.points_width.back() > best_candidate->points_width[1]) {
                polyline.points_width.back() = std::min(polyline.points_width[polyline.points_width.size() - 2], best_candidate->points_width[1]);
            }
            //be far enough
            int far_idx = 1;
            while (far_idx < best_candidate->points.size() && polyline.last_point().coincides_with_epsilon(best_candidate->points[far_idx]))
                far_idx++;
            polyline.points.insert(polyline.points.end(), best_candidate->points.begin() + far_idx, best_candidate->points.end());
            polyline.points_width.insert(polyline.points_width.end(), best_candidate->points_width.begin() + far_idx, best_candidate->points_width.end());
            polyline.endpoints.second = best_candidate->endpoints.second;
            assert(polyline.points_width.size() == polyline.points.size());
            deleted.insert(best_idx);
        }
    }
    //delete items to delete (iterate in reverse order to not invalidate the idxs
    for (auto rit = deleted.rbegin(); rit != deleted.rend(); rit++)
        pp.erase(pp.begin() + (*rit));
}

void
MedialAxis::concatenate_polylines_with_crossing(ThickPolylines& pp)
{
    // concatenate, but even where multiple thickpolyline join, to create nice long strait polylines
    /*  If we removed any short polylines we now try to connect consecutive polylines
    in order to allow loop detection. Note that this algorithm is greedier than
    MedialAxis::process_edge_neighbors() as it will connect random pairs of
    polylines even when more than two start from the same point. This has no
    drawbacks since we optimize later using nearest-neighbor which would do the
    same, but should we use a more sophisticated optimization algorithm we should
    not connect polylines when more than two meet.
    Optimisation of the old algorithm : now we select the most "strait line" choice
    when we merge with an other line at a point with more than two meet.
    */
    for (size_t i = 0; i < pp.size(); ++i) {
        ThickPolyline& polyline = pp[i];
        if (polyline.endpoints.first && polyline.endpoints.second) continue; // optimization

        ThickPolyline* best_candidate = nullptr;
        float best_dot = -1;
        size_t best_idx = 0;

        // find another polyline starting here
        for (size_t j = 0; j < pp.size(); ++j) {
            if (j == i) continue;
            ThickPolyline& other = pp[j];
            if (other.endpoints.first && other.endpoints.second) continue;
            bool me_reverse = false;
            bool other_reverse = false;
            if (polyline.last_point().coincides_with_epsilon(other.last_point())) {
                other_reverse = true;
            } else if (polyline.first_point().coincides_with_epsilon(other.last_point())) {
                me_reverse = true;
                other_reverse = true;
            } else if (polyline.first_point().coincides_with_epsilon(other.first_point())) {
                me_reverse = true;
            } else if (!polyline.last_point().coincides_with_epsilon(other.first_point())) {
                continue;
            }

            Vec2d v_poly(me_reverse ? polyline.lines().front().vector().x() : polyline.lines().back().vector().x(),
                me_reverse ? polyline.lines().front().vector().y() : polyline.lines().back().vector().y());
            v_poly *= (1 / std::sqrt(v_poly.x() * v_poly.x() + v_poly.y() * v_poly.y()));
            Vec2d v_other(other_reverse ? other.lines().back().vector().x() : other.lines().front().vector().x(),
                other_reverse ? other.lines().back().vector().y() : other.lines().front().vector().y());
            v_other *= (1 / std::sqrt(v_other.x() * v_other.x() + v_other.y() * v_other.y()));
            float other_dot = std::abs(float(v_poly.x() * v_other.x() + v_poly.y() * v_other.y()));
            if (other_dot > best_dot) {
                best_candidate = &other;
                best_idx = j;
                best_dot = other_dot;
            }
        }
        if (best_candidate != nullptr && best_candidate->points.size() > 1) {
            if (polyline.last_point().coincides_with_epsilon(best_candidate->last_point())) {
                best_candidate->reverse();
            } else if (polyline.first_point().coincides_with_epsilon(best_candidate->last_point())) {
                polyline.reverse();
                best_candidate->reverse();
            } else if (polyline.first_point().coincides_with_epsilon(best_candidate->first_point())) {
                polyline.reverse();
            }
            //intersections may create over-extrusion because the included circle can be a bit larger. We have to make it short again if needed.
            if (polyline.points.size() > 1 && best_candidate->points.size() > 1
                && polyline.points_width.back() > polyline.points_width[polyline.points_width.size() - 2]
                && polyline.points_width.back() > best_candidate->points_width[1]) {
                polyline.points_width.back() = std::min(polyline.points_width[polyline.points_width.size() - 2], best_candidate->points_width[1]);
            }
            //be far enough
            int far_idx = 1;
            while (far_idx < best_candidate->points.size() && polyline.last_point().coincides_with_epsilon(best_candidate->points[far_idx]))
                far_idx++;
            polyline.points.insert(polyline.points.end(), best_candidate->points.begin() + far_idx, best_candidate->points.end());
            polyline.points_width.insert(polyline.points_width.end(), best_candidate->points_width.begin() + far_idx, best_candidate->points_width.end());
            polyline.endpoints.second = best_candidate->endpoints.second;
            assert(polyline.points_width.size() == polyline.points.size());
            if (best_idx < i) i--;
            pp.erase(pp.begin() + best_idx);
        }
    }
}

void
MedialAxis::remove_too_thin_points(ThickPolylines& pp)
{
    //remove too thin polylines points (inside a polyline : split it)
    for (size_t i = 0; i < pp.size(); ++i) {
        ThickPolyline* polyline = &pp[i];

        // remove bits with too small extrusion
        size_t idx_point = 0;
        while (idx_point < polyline->points.size()) {
            if (polyline->points_width[idx_point] < this->m_min_width) {
                if (idx_point == 0) {
                    //too thin at start
                    polyline->points.erase(polyline->points.begin());
                    polyline->points_width.erase(polyline->points_width.begin());
                    idx_point = 0;
                } else if (idx_point == 1) {
                    //too thin at start
                    polyline->points.erase(polyline->points.begin());
                    polyline->points_width.erase(polyline->points_width.begin());
                    polyline->points.erase(polyline->points.begin());
                    polyline->points_width.erase(polyline->points_width.begin());
                    idx_point = 0;
                } else if (idx_point == polyline->points.size() - 2) {
                    //too thin at (near) end
                    polyline->points.erase(polyline->points.end() - 1);
                    polyline->points_width.erase(polyline->points_width.end() - 1);
                    polyline->points.erase(polyline->points.end() - 1);
                    polyline->points_width.erase(polyline->points_width.end() - 1);
                } else if (idx_point == polyline->points.size() - 1) {
                    //too thin at end
                    polyline->points.erase(polyline->points.end() - 1);
                    polyline->points_width.erase(polyline->points_width.end() - 1);
                } else {
                    //too thin in middle : split
                    pp.emplace_back();
                    polyline = &pp[i]; // have to refresh the pointer, as the emplace_back() may have moved the array
                    ThickPolyline& newone = pp.back();
                    newone.points.insert(newone.points.begin(), polyline->points.begin() + idx_point + 1, polyline->points.end());
                    newone.points_width.insert(newone.points_width.begin(), polyline->points_width.begin() + idx_point + 1, polyline->points_width.end());
                    polyline->points.erase(polyline->points.begin() + idx_point, polyline->points.end());
                    polyline->points_width.erase(polyline->points_width.begin() + idx_point, polyline->points_width.end());
                }
            } else idx_point++;

            if (polyline->points.size() < 2) {
                //remove self if too small
                pp.erase(pp.begin() + i);
                --i;
                break;
            }
        }
    }
}

void
MedialAxis::remove_too_thick_points(ThickPolylines& pp)
{
    if (m_biggest_width <= 0) return;
    //remove too thin polylines points (inside a polyline : split it)
    for (size_t i = 0; i < pp.size(); ++i) {
        ThickPolyline* polyline = &pp[i];

        // remove bits with too small extrusion
        size_t idx_point = 0;
        while (idx_point < polyline->points.size()) {
            if (polyline->points_width[idx_point] > m_biggest_width) {
                if (idx_point == 0) {
                    //too thin at start
                    polyline->points.erase(polyline->points.begin());
                    polyline->points_width.erase(polyline->points_width.begin());
                    idx_point = 0;
                } else if (idx_point == 1) {
                    //too thin at start
                    polyline->points.erase(polyline->points.begin());
                    polyline->points_width.erase(polyline->points_width.begin());
                    polyline->points.erase(polyline->points.begin());
                    polyline->points_width.erase(polyline->points_width.begin());
                    idx_point = 0;
                } else if (idx_point == polyline->points.size() - 2) {
                    //too thin at (near) end
                    polyline->points.erase(polyline->points.end() - 1);
                    polyline->points_width.erase(polyline->points_width.end() - 1);
                    polyline->points.erase(polyline->points.end() - 1);
                    polyline->points_width.erase(polyline->points_width.end() - 1);
                } else if (idx_point == polyline->points.size() - 1) {
                    //too thin at end
                    polyline->points.erase(polyline->points.end() - 1);
                    polyline->points_width.erase(polyline->points_width.end() - 1);
                } else {
                    //too thin in middle : split
                    pp.emplace_back();
                    polyline = &pp[i]; // have to refresh the pointer, as the emplace_back() may have moved the array
                    ThickPolyline& newone = pp.back();
                    newone.points.insert(newone.points.begin(), polyline->points.begin() + idx_point + 1, polyline->points.end());
                    newone.points_width.insert(newone.points_width.begin(), polyline->points_width.begin() + idx_point + 1, polyline->points_width.end());
                    polyline->points.erase(polyline->points.begin() + idx_point, polyline->points.end());
                    polyline->points_width.erase(polyline->points_width.begin() + idx_point, polyline->points_width.end());
                }
            } else idx_point++;

            if (polyline->points.size() < 2) {
                //remove self if too small
                pp.erase(pp.begin() + i);
                --i;
                break;
            }
        }
    }
}

void
MedialAxis::remove_too_short_polylines(ThickPolylines& pp)
{
    // reduce the flow at the intersection ( + ) points
    //FIXME: TODO: note that crossings are unnafected right now. they may need a different codepath directly in their method
    //TODO: unit tests for that.
    //TODO: never triggered. ther's only the sections passed by crossing fusion that aren't edge-case and it's not treated by this. => comment for now
    //for each not-endpoint point
    //std::vector<bool> endpoint_not_used(pp.size() * 2, true);
    //for (size_t idx_endpoint = 0; idx_endpoint < endpoint_not_used.size(); idx_endpoint++) {
    //    ThickPolyline& polyline = pp[idx_endpoint / 2];
    //    //update endpoint_not_used if not seen before
    //    if (idx_endpoint % 2 == 0 && endpoint_not_used[idx_endpoint]) {
    //        //update
    //        endpoint_not_used[(idx_endpoint / 2)] = !polyline.endpoints.first;
    //        endpoint_not_used[(idx_endpoint / 2) + 1] = endpoint_not_used[(idx_endpoint / 2) + 1] && !polyline.endpoints.second;
    //    }
    //    if (endpoint_not_used[idx_endpoint]) {
    //        int nb_endpoints;
    //        Point pt = idx_endpoint % 2 == 0 ? polyline.first_point() : polyline.last_point();
    //        if (idx_endpoint % 2 == 0 && pt.coincides_with_epsilon(polyline.last_point())) {
    //            nb_endpoints++;
    //            endpoint_not_used[(idx_endpoint / 2) + 1] = false;
    //        }
    //        //good, now find other points
    //        for (size_t idx_other_pp = (idx_endpoint / 2) + 1; idx_other_pp < pp.size(); idx_other_pp++) {
    //            ThickPolyline& other = pp[idx_other_pp];
    //            if (pt.coincides_with_epsilon(other.first_point())) {
    //                nb_endpoints++;
    //                endpoint_not_used[idx_other_pp * 2] = false;
    //            }
    //            if (pt.coincides_with_epsilon(other.last_point())) {
    //                nb_endpoints++;
    //                endpoint_not_used[idx_other_pp * 2 + 1] = false;
    //            }
    //        }
    //        if (nb_endpoints < 3)
    //            continue;
    //        // reduce width accordingly
    //        float reduction = 2.f / nb_endpoints;
    //        std::cout << "reduce " << reduction << " points!\n";
    //        if (idx_endpoint % 2 == 0 ) {
    //            polyline.points_width.front() *= reduction;
    //            if(pt.coincides_with_epsilon(polyline.last_point()))
    //                polyline.points_width.back() *= reduction;
    //        } else {
    //            polyline.points_width.back() *= reduction;
    //        }
    //        //good, now find other points
    //        for (size_t idx_other_pp = (idx_endpoint / 2) + 1; idx_other_pp < pp.size(); idx_other_pp++) {
    //            ThickPolyline& other = pp[idx_other_pp];
    //            if (pt.coincides_with_epsilon(other.first_point())) {
    //                other.points_width.front() *= reduction;
    //            }
    //            if (pt.coincides_with_epsilon(other.last_point())) {
    //                other.points_width.back() *= reduction;
    //            }
    //        }
    //        //TODO: restore good width at width dist, or reduce other points up to width dist
    //    }
    //}

    //remove too short polyline
    bool changes = true;
    while (changes) {
        changes = false;

        coordf_t shortest_size = (coordf_t)this->m_min_length;
        size_t shortest_idx = -1;
        for (size_t i = 0; i < pp.size(); ++i) {
            ThickPolyline& polyline = pp[i];
            // Remove the shortest polylines : polyline that are shorter than wider
            // (we can't do this check before endpoints extension and clipping because we don't
            // know how long will the endpoints be extended since it depends on polygon thickness
            // which is variable - extension will be <= m_max_width/2 on each side) 
            if ((polyline.endpoints.first || polyline.endpoints.second)) {
                coordf_t local_min_length = this->m_max_width / 2;
                for (coordf_t w : polyline.points_width)
                    local_min_length = std::max(local_min_length, w - SCALED_EPSILON);
                local_min_length = std::max(local_min_length, shortest_size);
                if (polyline.length() < local_min_length) {
                    if (shortest_size > polyline.length()) {
                        shortest_size = polyline.length();
                        shortest_idx = i;
                    }
                }
            }
        }
        if (shortest_idx < pp.size()) {
            pp.erase(pp.begin() + shortest_idx);
            changes = true;
        }
        if (changes) concatThickPolylines(pp);
    }

    //remove points too near each other
    changes = true;
    while (changes) {
        changes = false;
        size_t shortest_idx = -1;
        for (size_t polyidx = 0; polyidx < pp.size(); ++polyidx) {
            ThickPolyline& tp = pp[polyidx];
            for (size_t pt_idx = 1; pt_idx < tp.points.size() - 1; pt_idx++) {
                if (tp.points[pt_idx - 1].coincides_with_epsilon(tp.points[pt_idx])) {
                    tp.points.erase(tp.points.begin() + pt_idx);
                    tp.points_width.erase(tp.points_width.begin() + pt_idx);
                    pt_idx--;
                    changes = true;
                }
            }
            //check last segment
            if (tp.points.size() > 2 && tp.points[tp.points.size() - 2].coincides_with_epsilon(tp.points.back())) {
                tp.points.erase(tp.points.end() - 2);
                tp.points_width.erase(tp.points_width.end() - 2);
                changes = true;
            }
            //delete null-length polylines
            if (tp.length() < SCALED_EPSILON && tp.first_point().coincides_with_epsilon(tp.last_point())) {
                pp.erase(pp.begin() + polyidx);
                --polyidx;
                changes = true;
            }
        }
        if (changes) concatThickPolylines(pp);
    }

}

void
MedialAxis::check_width(ThickPolylines& pp, coord_t local_max_width, std::string msg)
{
    //remove empty polyline
    int nb = 0;
    for (size_t i = 0; i < pp.size(); ++i) {
        for (size_t j = 0; j < pp[i].points_width.size(); ++j) {
            if (pp[i].points_width[j] > coord_t(local_max_width * 1.01)) {
                BOOST_LOG_TRIVIAL(error) << "Error " << msg << " width " << unscaled(pp[i].points_width[j]) << "(" << i << ":" << j << ") > " << unscaled(local_max_width) << "\n";
                nb++;
            }
        }
    }
    if (nb > 0) BOOST_LOG_TRIVIAL(error) << "== nbBig = " << nb << " ==\n";
}

void
MedialAxis::ensure_not_overextrude(ThickPolylines& pp)
{
    //ensure the volume extruded is correct for what we have been asked
    // => don't over-extrude
    double surface = 0;
    double volume = 0;
    for (ThickPolyline& polyline : pp) {
        for (ThickLine& l : polyline.thicklines()) {
            surface += l.length() * (l.a_width + l.b_width) / 2;
            coord_t width_mean = (l.a_width + l.b_width) / 2;
            volume += this->m_height * (width_mean - this->m_height * (1. - 0.25 * PI)) * l.length();
        }
    }

    // compute bounds volume
    double boundsVolume = 0;
    boundsVolume += this->m_height * this->m_bounds->area();
    // add external "perimeter gap"
    double perimeterRoundGap = this->m_bounds->contour.length() * this->m_height * (1 - 0.25 * PI) * 0.5;
    // add holes "perimeter gaps"
    double holesGaps = 0;
    for (const Polygon& hole : this->m_bounds->holes) {
        holesGaps += hole.length() * this->m_height * (1 - 0.25 * PI) * 0.5;
    }
    boundsVolume += perimeterRoundGap + holesGaps;

    if (boundsVolume < volume) {
        //reduce width
        double reduce_by = boundsVolume / volume;
        for (ThickPolyline& polyline : pp) {
            for (coord_t& width : polyline.points_width) {
                width = coord_t(double(width) * reduce_by);
            }
        }
    }
}

void
MedialAxis::simplify_polygon_frontier()
{
    //it will remove every point in the surface contour that aren't on the bounds contour
    this->m_expolygon = this->m_surface;
    this->m_expolygon.contour.remove_collinear_angle(M_PI/180);
    for (Polygon& hole : this->m_expolygon.holes)
        hole.remove_collinear_angle(M_PI / 180);
    if (&this->m_surface != this->m_bounds) {
        bool need_intersect = false;
        for (size_t i = 0; i < this->m_expolygon.contour.points.size(); i++) {
            Point& p_check = this->m_expolygon.contour.points[i];
            //if (!find) {
            if (!this->m_bounds->has_boundary_point(p_check)) {
                //check if we put it at a bound point instead of delete it
                size_t prev_i = i == 0 ? this->m_expolygon.contour.points.size() - 1 : (i - 1);
                size_t next_i = i == this->m_expolygon.contour.points.size() - 1 ? 0 : (i + 1);
                const Point* closest = this->m_bounds->contour.closest_point(p_check);
                if (closest != nullptr && closest->distance_to(p_check) + SCALED_EPSILON
                    < std::min(p_check.distance_to(this->m_expolygon.contour.points[prev_i]), p_check.distance_to(this->m_expolygon.contour.points[next_i])) / 2) {
                    p_check.x() = closest->x();
                    p_check.y() = closest->y();
                    need_intersect = true;
                } else {
                    this->m_expolygon.contour.points.erase(this->m_expolygon.contour.points.begin() + i);
                    i--;
                }
            }
        }
        if (need_intersect) {
            ExPolygons simplified_polygons = intersection_ex(this->m_expolygon, *this->m_bounds);
            if (simplified_polygons.size() == 1) {
                this->m_expolygon = simplified_polygons[0];
            } else {
                //can't simplify that much, reuse the given one
                this->m_expolygon = this->m_surface;
                this->m_expolygon.contour.remove_collinear_angle(M_PI / 180);
                for (Polygon& hole : this->m_expolygon.holes)
                    hole.remove_collinear_angle(M_PI / 180);
            }
        }
    }

    if (!this->m_expolygon.contour.points.empty())
        this->m_expolygon.remove_point_too_near(this->m_resolution);
}

/// Grow the extrusion to at least nozzle_diameter*1.05 (lowest safe extrusion width)
/// Do not grow points inside the anchor.
void
MedialAxis::grow_to_nozzle_diameter(ThickPolylines& pp, const ExPolygons& anchors)
{
    //compute the min width
    coord_t min_width = this->m_nozzle_diameter;
    if (this->m_height > 0) min_width = Flow::new_from_spacing(
        float(unscaled(this->m_nozzle_diameter)),
        float(unscaled(this->m_nozzle_diameter)),
        float(unscaled(this->m_height)),
        1, false).scaled_width();
    //ensure the width is not lower than min_width.
    for (ThickPolyline& polyline : pp) {
        for (int i = 0; i < polyline.points.size(); ++i) {
            bool is_anchored = false;
            for (const ExPolygon& poly : anchors) {
                if (poly.contains(polyline.points[i])) {
                    is_anchored = true;
                    break;
                }
            }
            if (!is_anchored && polyline.points_width[i] < min_width)
                polyline.points_width[i] = min_width;
        }
    }
}

void
MedialAxis::taper_ends(ThickPolylines& pp)
{
    // minimum size of the taper: be sure to extrude at least the "round edges" of the extrusion (0-spacing extrusion).
    const coord_t min_size = (coord_t)std::max(this->m_nozzle_diameter * 0.1, this->m_height * (1. - 0.25 * PI));
    const coordf_t length = (coordf_t)std::min(this->m_taper_size, (this->m_nozzle_diameter - min_size) / 2);
    if (length <= coordf_t(this->m_resolution)) return;
    //ensure the width is not lower than min_size.
    for (ThickPolyline& polyline : pp) {
        if (polyline.length() < length * 2.2) continue;
        if (polyline.endpoints.first) {
            polyline.points_width[0] = min_size;
            coord_t current_dist = min_size;
            coord_t last_dist = min_size;
            for (size_t i = 1; i < polyline.points_width.size(); ++i) {
                current_dist += (coord_t)polyline.points[i - 1].distance_to(polyline.points[i]);
                if (current_dist > length) {
                    //create a new point if not near enough
                    if (current_dist > length + coordf_t(this->m_resolution)) {
                        coordf_t percent_dist = (length - last_dist) / (current_dist - last_dist);
                        polyline.points.insert(polyline.points.begin() + i, polyline.points[i - 1].interpolate(percent_dist, polyline.points[i]));
                        polyline.points_width.insert(polyline.points_width.begin() + i, polyline.points_width[i]);
                    }
                    break;
                }
                polyline.points_width[i] = std::max((coordf_t)min_size, min_size + (polyline.points_width[i] - min_size) * current_dist / length);
                last_dist = current_dist;
            }
        }
        if (polyline.endpoints.second) {
            polyline.points_width[polyline.points_width.size() - 1] = min_size;
            coord_t current_dist = min_size;
            coord_t last_dist = min_size;
            for (size_t i = polyline.points_width.size() - 1; i > 0; --i) {
                current_dist += (coord_t)polyline.points[i].distance_to(polyline.points[i - 1]);
                if (current_dist > length) {
                    //create new point if not near enough
                    if (current_dist > length + coordf_t(this->m_resolution)) {
                        coordf_t percent_dist = (length - last_dist) / (current_dist - last_dist);
                        polyline.points.insert(polyline.points.begin() + i, polyline.points[i].interpolate(percent_dist, polyline.points[i - 1]));
                        polyline.points_width.insert(polyline.points_width.begin() + i, polyline.points_width[i - 1]);
                    }
                    break;
                }
                polyline.points_width[i - 1] = std::max((coordf_t)min_size, min_size + (polyline.points_width[i - 1] - min_size) * current_dist / length);
                last_dist = current_dist;
            }
        }
    }
}

double
check_circular(ExPolygon& expolygon, coord_t max_variation) {
    if (expolygon.holes.size() > 0) return 0;

    //test if convex
    if (expolygon.contour.concave_points().empty() && expolygon.contour.points.size() > 3) {
        // Computing circle center
        Point center = expolygon.contour.centroid();
        coordf_t radius_min = std::numeric_limits<float>::max(), radius_max = 0;
        for (int i = 0; i < expolygon.contour.points.size(); ++i) {
            coordf_t dist = expolygon.contour.points[i].distance_to(center);
            radius_min = std::min(radius_min, dist);
            radius_max = std::max(radius_max, dist);
        }
        // check with max_variation to be sure it's round enough
        if (radius_max - radius_min < max_variation) {
            return radius_max;
        }
    }
    return 0;
}

void
MedialAxis::build(ThickPolylines& polylines_out)
{
    //static int id = 0;
    //id++;
    //std::cout << id << "\n";
    //{
    //    std::stringstream stri;
    //    stri << "medial_axis_0_enter_" << id << ".svg";
    //    SVG svg(stri.str());
    //    svg.draw(this->m_surface);
    //    svg.Close();
    //}
    simplify_polygon_frontier();
    //{
    //    std::stringstream stri;
    //    stri << "medial_axis_0.5_simplified_" << id << ".svg";
    //    SVG svg(stri.str());
    //    svg.draw(*this->m_bounds, "grey");
    //    svg.draw(this->m_expolygon, "green");
    //    svg.Close();
    //}
    //safety check
    if (this->m_expolygon.area() < this->m_min_width * this->m_min_width) this->m_expolygon = this->m_surface;
    if (this->m_expolygon.area() < this->m_min_width * this->m_min_width) return;

    //check for circular shape
    coordf_t radius = check_circular(this->m_expolygon, this->m_min_width / 4);
    if (radius > 0 && this->m_expolygon.contour.points.size() > 4) {
        ExPolygons miniPeri = offset_ex(Polygons{ this->m_expolygon.contour }, -radius / 2);
        if (miniPeri.size() == 1 && miniPeri[0].holes.size() == 0) {
            ThickPolyline thickPoly;
            thickPoly.points = miniPeri[0].contour.points;
            thickPoly.points.push_back(thickPoly.points.front());
            thickPoly.endpoints.first = false;
            thickPoly.endpoints.second = false;
            for (int i = 0; i < thickPoly.points.size(); i++) {
                thickPoly.points_width.push_back(radius);
            }
            polylines_out.insert(polylines_out.end(), thickPoly);
            return;
        }
    }

    //std::cout << "simplify_polygon_frontier\n";
    // compute the Voronoi diagram and extract medial axis polylines
    ThickPolylines pp;
    this->polyline_from_voronoi(this->m_expolygon, &pp);
    //FIXME this is a stop-gap for voronoi bug, see superslicer/issues/995
    {
        double ori_area = 0;
        for (ThickPolyline& tp : pp) {
            for (int i = 1; i < tp.points.size(); i++) {
                ori_area += (tp.points_width[i - 1] + tp.points_width[i]) * tp.points[i - 1].distance_to(tp.points[i]) / 2;
            }
        }
        double area = this->m_expolygon.area();
        double ratio_area = ori_area / area;
        if (ratio_area < 1) ratio_area = 1 / ratio_area;
        //check if the returned voronoi is really off
        if (ratio_area > 1.1) {
            //add a little offset and retry
            ExPolygons fixer = offset_ex(this->m_expolygon, SCALED_EPSILON);
            if (fixer.size() == 1) {
                ExPolygon fixPoly = fixer[0];
                ThickPolylines pp_stopgap;
                this->polyline_from_voronoi(fixPoly, &pp_stopgap);
                double fix_area = 0;
                for (ThickPolyline& tp : pp_stopgap) {
                    for (int i = 1; i < tp.points.size(); i++) {
                        fix_area += (tp.points_width[i - 1] + tp.points_width[i]) * tp.points[i - 1].distance_to(tp.points[i]) / 2;
                    }
                }
                double fix_ratio_area = fix_area / area;
                if (fix_ratio_area < 1) fix_ratio_area = 1 / fix_ratio_area;
                //if it's less off, then use it.
                if (fix_ratio_area < ratio_area) {
                    pp = pp_stopgap;
                }
            }
        }
    }
    //{
    //    std::stringstream stri;
    //    stri << "medial_axis_0.9_voronoi_" << id << ".svg";
    //    SVG svg(stri.str());
    //    svg.draw(*this->m_bounds, "grey");
    //    svg.draw(this->m_expolygon, "green");
    //    svg.draw(pp, "red");
    //    svg.Close();
    //}

    //sanity check, as the voronoi can return (abeit very rarely) randomly high values.
    for (size_t tp_idx = 0; tp_idx < pp.size(); tp_idx++) {
        ThickPolyline& tp = pp[tp_idx];
        for (size_t i = 0; i < tp.points_width.size(); i++) {
            if (tp.points_width[i] > this->m_max_width) {
                tp.points_width[i] = this->m_max_width;
            }
        }
        // voronoi bugfix: when we have a wheel, it creates a polyline at the center, completly out of the polygon. #651
        // note: can't reproduce in the new verison. This may have been fixed by another way.
        //if (tp.endpoints.first && tp.endpoints.second && !this->m_expolygon.contains(tp.first_point()) && !this->m_expolygon.contains(tp.last_point()) && pp.size() > 1) {
        //    //delete this out-of-bounds polyline
        //    pp.erase(pp.begin() + tp_idx);
        //    --tp_idx;
        //}
        //voronoi problem: can put two consecutive points at the same position. Delete one.
        for (size_t i = 1; i < tp.points.size() - 1; i++) {
            if (tp.points[i - 1].distance_to_square(tp.points[i]) < SCALED_EPSILON) {
                tp.points.erase(tp.points.begin() + i);
                tp.points_width.erase(tp.points_width.begin() + i);
                i--;
            }
        }
        //delete the inner one
        if (tp.points.size() > 2 && tp.points[tp.points.size() - 2].distance_to_square(tp.points.back()) < SCALED_EPSILON) {
            tp.points.erase(tp.points.end() - 2);
            tp.points_width.erase(tp.points_width.end() - 2);
        }
        //delete null-length polylines
        if (tp.length() < SCALED_EPSILON && tp.first_point().coincides_with_epsilon(tp.last_point())) {
            pp.erase(pp.begin() + tp_idx);
            --tp_idx;
        }
    }
    //std::cout << "polyline_from_voronoi\n";
    //{
    //    std::stringstream stri;
    //    stri << "medial_axis_1_voronoi_" << id << ".svg";
    //    SVG svg(stri.str());
    //    svg.draw(*this->m_bounds, "grey");
    //    svg.draw(this->m_expolygon, "green");
    //    svg.draw(pp, "red");
    //    svg.Close();
    //}

    //check_width(pp, this->m_max_width, "polyline_from_voronoi");

    concatThickPolylines(pp);

    //std::cout << "concatThickPolylines\n";
    //{
    //    std::stringstream stri;
    //    stri << "medial_axis_1_voronoi_" << id << ".svg";
    //    SVG svg(stri.str());
    //    svg.draw(*this->m_bounds, "grey");
    //    svg.draw(this->m_expolygon, "green");
    //    svg.draw(pp, "red");
    //    svg.Close();
    //}

    /* Find the maximum width returned; we're going to use this for validating and
       filtering the output segments. */
    coord_t max_w = 0;
    for (ThickPolylines::const_iterator it = pp.begin(); it != pp.end(); ++it)
        max_w = std::max(max_w, (coord_t)*std::max_element(it->points_width.begin(), it->points_width.end()));

    //for (auto &p : pp) {
    //    std::cout << "Start polyline : ";
    //    for (auto &w : p.width) {
    //        std::cout << ", " << w;
    //    }
    //    std::cout << "\n";
    //}

    // "remove" the little paths that are at the outside of a curve.
    fusion_curve(pp);
    //{
    //    std::stringstream stri;
    //    stri << "medial_axis_2_curve_" << id << ".svg";
    //    SVG svg(stri.str());
    //    svg.draw(*this->m_bounds, "grey");
    //    svg.draw(this->m_expolygon, "green");
    //    svg.draw(pp, "red");
    //    svg.Close();
    //}


    // Aligned fusion: Fusion the bits at the end of lines by "increasing thickness"
    // For that, we have to find other lines,
    // and with a next point no more distant than the max width.
    // Then, we can merge the bit from the first point to the second by following the mean.
    //
    main_fusion(pp);
    //{
    //    std::stringstream stri;
    //    stri << "medial_axis_3_fusion_" << id << ".svg";
    //    SVG svg(stri.str());
    //    svg.draw(*this->m_bounds, "grey");
    //    svg.draw(this->m_expolygon, "green");
    //    svg.draw(pp, "red");
    //    svg.Close();
    //}

    //fusion right-angle corners.
    fusion_corners(pp);

    // Loop through all returned polylines in order to extend their endpoints to the 
    //   expolygon boundaries (if done here, it may be cut later if not thick enough)
    if (m_stop_at_min_width) {
        //{
        //    std::stringstream stri;
        //    stri << "medial_axis_3_3_extends_" << id << ".svg";
        //    SVG svg(stri.str());
        //    svg.draw(*this->m_bounds, "grey");
        //    svg.draw(this->m_expolygon, "green");
        //    svg.draw(pp, "red");
        //    svg.Close();
        //}
        extends_line_both_side(pp);
    }

    /*for (auto &p : pp) {
        std::cout << "Fusion polyline : ";
        for (auto &w : p.width) {
            std::cout << ", " << w;
        }
        std::cout << "\n";
    }*/
    //reduce extrusion when it's too thin to be printable
    //{
    //    std::stringstream stri;
    //    stri << "medial_axis_3_6_remove_thin_" << id << ".svg";
    //    SVG svg(stri.str());
    //    svg.draw(*this->m_bounds, "grey");
    //    svg.draw(this->m_expolygon, "green");
    //    svg.draw(pp, "red");
    //    svg.Close();
    //}

    remove_too_thin_extrusion(pp);
    //{
    //    std::stringstream stri;
    //    stri << "medial_axis_4_thinok_" << id << ".svg";
    //    SVG svg(stri.str());
    //    svg.draw(*this->m_bounds, "grey");
    //    svg.draw(this->m_expolygon, "green");
    //    svg.draw(pp, "red");
    //    svg.Close();
    //}

    remove_too_thin_points(pp);
    remove_too_thick_extrusion(pp);
    //{
    //    std::stringstream stri;
    //    stri << "medial_axis_5.0_thuinner_" << id << ".svg";
    //    SVG svg(stri.str());
    //    svg.draw(*this->m_bounds, "grey");
    //    svg.draw(this->m_expolygon, "green");
    //    svg.draw(pp, "red");
    //    svg.Close();
    //}

    // Loop through all returned polylines in order to extend their endpoints to the 
    //   expolygon boundaries
    if (!m_stop_at_min_width) {
        extends_line_both_side(pp);
    }
    //{
    //    std::stringstream stri;
    //    stri << "medial_axis_5_expand_" << id << ".svg";
    //    SVG svg(stri.str());
    //    svg.draw(*this->m_bounds, "grey");
    //    svg.draw(this->m_expolygon, "green");
    //    svg.draw(pp, "red");
    //    svg.Close();
    //}
    //TODO: reduce the flow at the intersection ( + ) points on crossing?
    concatenate_small_polylines(pp);
    //{
    //    std::stringstream stri;
    //    stri << "medial_axis_6_concat_small_" << id << ".svg";
    //    SVG svg(stri.str());
    //    svg.draw(*this->m_bounds, "grey");
    //    svg.draw(this->m_expolygon, "green");
    //    svg.draw(pp, "red");
    //    svg.Close();
    //}

    concatenate_polylines_with_crossing(pp);
    //{
    //    std::stringstream stri;
    //    stri << "medial_axis_7_concat_" << id << ".svg";
    //    SVG svg(stri.str());
    //    svg.draw(*this->m_bounds, "grey");
    //    svg.draw(this->m_expolygon, "green");
    //    svg.draw(pp, "red");
    //    svg.Close();
    //}

    extends_line_extra(pp);

    remove_too_short_polylines(pp);
    //{
    //    std::stringstream stri;
    //    stri << "medial_axis_8_tooshort_" << id << ".svg";
    //    SVG svg(stri.str());
    //    svg.draw(*this->m_bounds, "grey");
    //    svg.draw(this->m_expolygon, "green");
    //    svg.draw(pp, "red");
    //    svg.Close();
    //}

    ensure_not_overextrude(pp);
    //{
    //    std::stringstream stri;
    //    stri << "medial_axis_9.1_end_" << id << ".svg";
    //    SVG svg(stri.str());
    //    svg.draw(*this->m_bounds, "grey");
    //    svg.draw(this->m_expolygon, "green");
    //    svg.draw(pp, "red");
    //    svg.Close();
    //}
    if (m_nozzle_diameter != this->m_min_width) {
        grow_to_nozzle_diameter(pp, diff_ex(*this->m_bounds, this->m_expolygon));
    }
    if (this->m_taper_size != 0) {
        taper_ends(pp);
    }
    //{
    //    std::stringstream stri;
    //    stri << "medial_axis_9.9_endnwithtaper_" << id << ".svg";
    //    SVG svg(stri.str());
    //    svg.draw(*this->m_bounds, "grey");
    //    svg.draw(this->m_expolygon, "green");
    //    svg.draw(pp, "red");
    //    svg.Close();
    //}

    remove_bits(pp);

    //sort_polylines(pp);

    //for (auto &p : pp) {
    //    std::cout << " polyline : ";
    //    for (auto &w : p.width) {
    //        std::cout << ", " << w;
    //    }
    //    std::cout << "\n";
    //}

    polylines_out.insert(polylines_out.end(), pp.begin(), pp.end());

}

ExtrusionMultiPath variable_width(const ThickPolyline& polyline, const ExtrusionRole role, const Flow& flow, const coord_t resolution_internal, const coord_t tolerance) {
    return ExtrusionMultiPath(unsafe_variable_width(polyline, role, flow, resolution_internal, tolerance));
}
ExtrusionPaths
unsafe_variable_width(const ThickPolyline& polyline, const ExtrusionRole role, const Flow& flow, const coord_t resolution_internal, const coord_t tolerance)
{

    ExtrusionPaths paths;
    ExtrusionPath path(role);
    ThickLines lines = polyline.thicklines();
    Flow current_flow = flow;

    coordf_t saved_line_len = 0;
    for (int i = 0; i < (int)lines.size(); ++i) {
        ThickLine& line = lines[i];

        const coordf_t line_len = line.length();
        const coordf_t prev_line_len = saved_line_len;
        saved_line_len = line_len;

        assert(line.a_width > SCALED_EPSILON && !std::isnan(line.a_width));
        assert(line.b_width > SCALED_EPSILON && !std::isnan(line.b_width));
        coord_t thickness_delta = std::abs(line.a_width - line.b_width);

        // split lines ?
        if (resolution_internal < line_len) {
            if (thickness_delta > tolerance && ceil(float(thickness_delta) / float(tolerance)) > 2) {
                const uint16_t segments = 1 + (uint16_t)std::min((uint32_t)16000, (uint32_t)ceil(float(thickness_delta) / float(tolerance)));
                Points pp;
                std::vector<coordf_t> width;
                {
                    for (size_t j = 0; j < segments; ++j) {
                        pp.push_back(line.a.interpolate(((double)j) / segments, line.b));
                        double percent_width = ((double)j) / (segments - 1);
                        width.push_back(line.a_width * (1 - percent_width) + line.b_width * percent_width);
                    }
                    pp.push_back(line.b);

                    assert(pp.size() == segments + 1);
                    assert(width.size() == segments);
                }

                // delete this line and insert new ones
                lines.erase(lines.begin() + i);
                for (size_t j = 0; j < segments; ++j) {
                    ThickLine new_line(pp[j], pp[j + 1]);
                    new_line.a_width = width[j];
                    new_line.b_width = width[j];
                    lines.insert(lines.begin() + i + j, new_line);
                }

                // go back to the start of this loop iteration
                --i;
                continue;
            } else if (thickness_delta > 0) {
                //create a middle point
                ThickLine new_line(line.a.interpolate(0.5, line.b), line.b);
                new_line.a_width = line.b_width;
                new_line.b_width = line.b_width;
                line.b = new_line.a;
                line.b_width = line.a_width;
                lines.insert(lines.begin() + i + 1, new_line);

                // go back to the start of this loop iteration
                --i;
                continue;
            }
        } else if (i > 0 && resolution_internal > line_len + prev_line_len) {
            //merge lines?
            //if it's a loop, merge only if the distance is high enough
            if (polyline.first_point() == polyline.last_point() && polyline.length() < (line_len + prev_line_len) * 6)
                continue;
            ThickLine& prev_line = lines[i - 1];
            coordf_t width = prev_line_len * (prev_line.a_width + prev_line.b_width) / 2;
            width += line_len * (line.a_width + line.b_width) / 2;
            prev_line.b = line.b;
            const coordf_t new_length = prev_line.length();
            if (new_length < SCALED_EPSILON) {
                // too short, remove it and restart
                if (i > 1) {
                    line.a = lines[i - 2].b;
                }
                lines.erase(lines.begin() + i - 1);
                i -= 2;
                continue;
            }
            width /= new_length;
            prev_line.a_width = width;
            prev_line.b_width = width;
            saved_line_len = new_length;
            //erase 'line'
            lines.erase(lines.begin() + i);
            --i;
            continue;
        } else if (thickness_delta > 0) {
            //set width as a middle-ground
            line.a_width = (line.a_width + line.b_width) / 2;
            line.b_width = line.a_width;
        }
    }
    for (int i = 0; i < (int)lines.size(); ++i) {
        ThickLine& line = lines[i];

        //gapfill : we want to be able to fill the voids (touching the perimeters), so the spacing is what we want.
        //thinwall: we want the extrusion to not go out of the polygon, so the width is what we want.
        //  but we can't extrude with a negative spacing, so we have to gradually fall back to spacing if the width is too small.

        // default: extrude a thin wall that doesn't go outside of the specified width.
        double wanted_width = unscaled(line.a_width);
        //if gapfill or arachne, the width is in fact the spacing.
        if (role != erThinWall) {
            if (role == erOverhangPerimeter && flow.bridge()) {
                // for Arachne overhangs: keep the bridge width.
                wanted_width = flow.width();
            } else {
                // Convert from spacing to extrusion width based on the extrusion model
                // of a square extrusion ended with semi circles.
                wanted_width = Flow::rounded_rectangle_extrusion_width_from_spacing(unscaled(line.a_width), flow.height(), flow.spacing_ratio());
            }
        }
        // check if the width isn't too small (negative spacing)
        // 1.f spacing ratio, because it's to get the really minimum. 0 spacing ratio will makes that irrelevant.
        if (unscale<coordf_t>(line.a_width) < 2 * Flow::rounded_rectangle_extrusion_width_from_spacing(0.f, flow.height(), 1.f)) {
            //width (too) small, be sure to not extrude with negative spacing.
            //we began to fall back to spacing gradually even before the spacing go into the negative
            //  to make extrusion1 < extrusion2 if width1 < width2 even if width2 is too small. 
            wanted_width = unscaled(line.a_width) * 0.35 + 1.3 * Flow::rounded_rectangle_extrusion_width_from_spacing(0.f, flow.height(), 1.f);
        }

        if (path.polyline.empty()) {
            if (wanted_width != current_flow.width()) {
                current_flow = current_flow.with_width((float)wanted_width);
            }
            path.polyline.append(line.a);
            path.polyline.append(line.b);
            assert(!std::isnan(current_flow.mm3_per_mm()));
            assert(!std::isnan(current_flow.width()));
            assert(!std::isnan(current_flow.height()));
            path.mm3_per_mm = current_flow.mm3_per_mm();
            path.width = current_flow.width();
            path.height = current_flow.height();
        } else {
            coord_t thickness_delta = scale_t(fabs(current_flow.width() - wanted_width));
            if (thickness_delta <= tolerance / 2) {
                // the width difference between this line and the current flow width is 
                // within the accepted tolerance
                path.polyline.append(line.b);
            } else {
                // we need to initialize a new line
                paths.push_back(path);
                path = ExtrusionPath(role);
                if (wanted_width != current_flow.width()) {
                    current_flow = current_flow.with_width(wanted_width);
                }
                path.polyline.append(line.a);
                path.polyline.append(line.b);
                assert(!std::isnan(current_flow.mm3_per_mm()));
                assert(!std::isnan(current_flow.width()));
                assert(!std::isnan(current_flow.height()));
                path.mm3_per_mm = current_flow.mm3_per_mm();
                path.width = current_flow.width();
                path.height = current_flow.height();
            }
        }
        assert(path.polyline.size() > 2 || path.first_point() != path.last_point());
    }
    if (path.polyline.is_valid())
        paths.push_back(path);

    return paths;
}

ExtrusionEntitiesPtr
    thin_variable_width(const ThickPolylines& polylines, const ExtrusionRole role, const Flow& flow, const coord_t resolution_internal)
{
    assert(resolution_internal > SCALED_EPSILON);

    // this value determines granularity of adaptive width, as G-code does not allow
    // variable extrusion within a single move; this value shall only affect the amount
    // of segments, and any pruning shall be performed before we apply this tolerance
    const coord_t tolerance = flow.scaled_width() / 10;//scale_(0.05);
    ExtrusionEntitiesPtr coll;
    for (const ThickPolyline& p : polylines) {
        ExtrusionMultiPath multi_paths = variable_width(p, role, flow, resolution_internal, tolerance);
        // Append paths to collection.
        if (!multi_paths.empty()) {
#if _DEBUG
            for (auto it = std::next(multi_paths.paths.begin()); it != multi_paths.paths.end(); ++it) {
                assert(it->polyline.size() >= 2);
                assert(std::prev(it)->polyline.back() == it->polyline.front());
            }
#endif
            if (multi_paths.paths.front().first_point().coincides_with_epsilon(multi_paths.paths.back().last_point())) {
                coll.push_back(new ExtrusionLoop(std::move(multi_paths.paths)));
            } else {
                if (role == erThinWall) {
                    //thin walls : avoid to cut them, please.
                    //also, keep the start, as the start should be already in a frontier where possible.
                    ExtrusionEntityCollection* unsortable_coll = new ExtrusionEntityCollection(std::move(multi_paths.paths));
                    unsortable_coll->set_can_sort_reverse(false, false);
                    //TODO un-reversable multipath ?
                    coll.push_back(unsortable_coll);
                } else if (role == erGapFill) {
                    if (multi_paths.size() == 1) {
                        coll.push_back(multi_paths.paths.front().clone_move());
                    } else {
                        //can reverse but not sort/cut: it's a multipath!
                        coll.push_back(multi_paths.clone_move());
                    }
                } else {
                    coll.push_back(multi_paths.clone_move());
                }
            }
        }
    }
    return coll;
}

} } // namespace Slicer::Geometry
