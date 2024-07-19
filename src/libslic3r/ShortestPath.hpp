///|/ Copyright (c) Prusa Research 2019 - 2023 Vojtěch Bubník @bubnikv, Lukáš Matěna @lukasmatena
///|/
///|/ PrusaSlicer is released under the terms of the AGPLv3 or higher
///|/
#ifndef slic3r_ShortestPath_hpp_
#define slic3r_ShortestPath_hpp_

#include "libslic3r.h"
#include "ExtrusionEntity.hpp"
#include "Point.hpp"

#include <utility>
#include <vector>

namespace Slic3r {

	namespace ClipperLib {
		class PolyNode;
		using PolyNodes = std::vector<PolyNode*, PointsAllocator<PolyNode*>>;
	}

class ExPolygon;
using ExPolygons = std::vector<ExPolygon>;

// Used by chain_expolygons()
std::vector<size_t> 				 chain_points(const Points &points, Point *start_near = nullptr);
// Used to give layer islands a print order.
std::vector<size_t> 				 chain_expolygons(const ExPolygons &expolygons, Point *start_near = nullptr);

// Chain extrusion entities by a shortest distance. Returns the ordered extrusions together with a "reverse" flag.
// Set input "reversed" to true if the vector of "entities" is to be considered to be reversed once already.
std::vector<std::pair<size_t, bool>> chain_extrusion_entities(const std::vector<ExtrusionEntity*> &entities, const Point *start_near = nullptr, const bool reversed = false);
std::vector<std::pair<size_t, bool>> chain_extrusion_entities(const std::vector<const ExtrusionEntity*> &entities, const Point *start_near = nullptr, const bool reversed = false);
// Reorder & reverse extrusion entities in place based on the "chain" ordering.
template<typename ExtrusionEntityConstOrNot>
// void reorder_extrusion_entities(std::vector<ExtrusionEntityConstOrNot*> &entities, const std::vector<std::pair<size_t, bool>> &chain)
// {
// 	assert(entities.size() == chain.size());
// 	std::vector<const ExtrusionEntity*> out;
// 	out.reserve(entities.size());
//     for (const std::pair<size_t, bool> &idx : chain) {
// 		assert(entities[idx.first] != nullptr);
//         out.emplace_back(entities[idx.first]);
//         if (idx.second)
// 			out.back()->reverse();
//     }
//     entities.swap(out);
// }
void reorder_extrusion_entities(std::vector<ExtrusionEntityConstOrNot*> &entities, const std::vector<std::pair<size_t, bool>> &chain)
{
    assert(entities.size() == chain.size());
    std::vector<ExtrusionEntity*> out;  // Change to non-const pointer
    out.reserve(entities.size());
    for (const std::pair<size_t, bool> &idx : chain) {
        assert(entities[idx.first] != nullptr);
        out.emplace_back(entities[idx.first]);
        if (idx.second)
            out.back()->reverse();
    }
    entities.swap(out);
}
// Reorder & reverse extrusion entities in place.
template<typename ExtrusionEntityConstOrNot>
void chain_and_reorder_extrusion_entities(std::vector<ExtrusionEntityConstOrNot*> &entities, const Point *start_near = nullptr)
{
	reorder_extrusion_entities(entities, chain_extrusion_entities(entities, start_near));
}

// Chain extrusion entities by a shortest distance. Returns the ordered extrusions together with a "reverse" flag.
// Set input "reversed" to true if the vector of "entities" is to be considered to be reversed.
template<typename ExtrusionEntityConstOrNot>
ExtrusionEntityReferences chain_extrusion_references(const std::vector<ExtrusionEntityConstOrNot*> &entities, const Point *start_near = nullptr, const bool reversed = false)
{
	const std::vector<std::pair<size_t, bool>> chain = chain_extrusion_entities(entities, start_near, reversed);
	ExtrusionEntityReferences out;
	out.reserve(chain.size());
    for (const std::pair<size_t, bool> &idx : chain) {
		assert(entities[idx.first] != nullptr);
        out.push_back({ *entities[idx.first], idx.second });
    }
    return out;
}

// The same as above, respect eec.no_sort flag.
ExtrusionEntityReferences			 chain_extrusion_references(const ExtrusionEntityCollection &eec, const Point *start_near = nullptr, const bool reversed = false);

std::vector<std::pair<size_t, bool>> chain_extrusion_paths(std::vector<ExtrusionPath> &extrusion_paths, const Point *start_near = nullptr);
void                                 reorder_extrusion_paths(std::vector<ExtrusionPath> &extrusion_paths, std::vector<std::pair<size_t, bool>> &chain);
void                                 chain_and_reorder_extrusion_paths(std::vector<ExtrusionPath> &extrusion_paths, const Point *start_near = nullptr);

Polylines 							 chain_polylines(Polylines &&src, const Point *start_near = nullptr);
inline Polylines 					 chain_polylines(const Polylines& src, const Point* start_near = nullptr) { Polylines tmp(src); return chain_polylines(std::move(tmp), start_near); }

ClipperLib::PolyNodes				 chain_clipper_polynodes(const Points &points, const ClipperLib::PolyNodes &items);

// Chain instances of print objects by an approximate shortest path.
// Returns pairs of PrintObject idx and instance of that PrintObject.
class Print;
struct PrintInstance;
std::vector<const PrintInstance*> 	 chain_print_object_instances(const Print &print);

// Chain lines into polylines.
Polylines 							 chain_lines(const std::vector<Line> &lines, const double point_distance_epsilon);

} // namespace Slic3r

#endif /* slic3r_ShortestPath_hpp_ */
