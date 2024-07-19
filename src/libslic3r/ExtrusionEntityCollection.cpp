///|/ Copyright (c) Prusa Research 2016 - 2023 Vojtěch Bubník @bubnikv, Lukáš Hejl @hejllukas, Lukáš Matěna @lukasmatena
///|/ Copyright (c) SuperSlicer 2023 Remi Durand @supermerill
///|/ Copyright (c) Slic3r 2013 - 2016 Alessandro Ranellucci @alranel
///|/ Copyright (c) 2015 Maksim Derbasov @ntfshard
///|/ Copyright (c) 2014 Petr Ledvina @ledvinap
///|/
///|/ PrusaSlicer is released under the terms of the AGPLv3 or higher
///|/
#include "ExtrusionEntityCollection.hpp"
#include "ShortestPath.hpp"
#include <algorithm>
#include <cmath>
#include <map>

namespace Slic3r {

#if 0
void filter_by_extrusion_role_in_place(ExtrusionEntitiesPtr &extrusion_entities, ExtrusionRole role)
{
	if (role != ExtrusionRole::Mixed) {
		auto first  = extrusion_entities.begin();
		auto last   = extrusion_entities.end();
        extrusion_entities.erase(
            std::remove_if(first, last, [&role](const ExtrusionEntity* ee) {
                return ee->role() != role; }),
            last);
	}
}
#endif

ExtrusionEntityCollection::ExtrusionEntityCollection(const ExtrusionPaths &paths)
    : m_no_sort(false), ExtrusionEntity(true)
{
    this->append(paths);
}

ExtrusionEntityCollection& ExtrusionEntityCollection::operator= (const ExtrusionEntityCollection &other)
{
    this->m_no_sort = other.m_no_sort;
    this->m_can_reverse = other.m_can_reverse;
    clear();
    this->append(other.m_entities);
    return *this;
}

void ExtrusionEntityCollection::swap(ExtrusionEntityCollection &c)
{
    std::swap(this->m_entities, c.m_entities);
    std::swap(this->m_no_sort, c.m_no_sort);
    std::swap(this->m_can_reverse, c.m_can_reverse);
}

void ExtrusionEntityCollection::clear()
{
	for (size_t i = 0; i < this->m_entities.size(); ++i)
		delete this->m_entities[i];
    this->m_entities.clear();
}

ExtrusionEntityCollection::operator ExtrusionPaths() const
{
    ExtrusionPaths paths;
    for (const ExtrusionEntity *ptr : this->entities()) {
        if (const ExtrusionPath *path = dynamic_cast<const ExtrusionPath*>(ptr))
            paths.push_back(*path);
    }
    return paths;
}

void ExtrusionEntityCollection::reverse()
{
    for (ExtrusionEntity *ptr : this->m_entities)
    {
        // Don't reverse it if it's a loop, as it doesn't change anything in terms of elements ordering
        // and caller might rely on winding order
        if (ptr->can_reverse() && !ptr->is_loop())
            ptr->reverse();
    }
    std::reverse(this->m_entities.begin(), this->m_entities.end());
}

void ExtrusionEntityCollection::replace(size_t i, const ExtrusionEntity &entity)
{
    delete this->m_entities[i];
    this->m_entities[i] = entity.clone();
}

void ExtrusionEntityCollection::remove(size_t i)
{
    delete this->m_entities[i];
    this->m_entities.erase(this->m_entities.begin() + i);
}

// note: chained_path_from only this collection. You still need to chained_path_from the child collections.
ExtrusionEntityReferences ExtrusionEntityCollection::chained_path_from(const Point &start_near)
{
    if (this->m_no_sort) {
        ExtrusionEntityReferences result{};
        bool need_reverse = false;
        if (this->m_can_reverse) {
            if (!m_entities.empty()) {
                if (m_entities.front()->is_collection()) {
                    assert(dynamic_cast<ExtrusionEntityCollection *>(m_entities.front()) != nullptr);
                    ExtrusionEntityCollection *front_coll = static_cast<ExtrusionEntityCollection *>(
                        m_entities.front());
                    result = front_coll->chained_path_from(start_near);
                    assert(!result.empty());
                } else if (m_entities.front()->can_reverse() &&
                           m_entities.front()->first_point().distance_to_square(start_near) >
                               m_entities.front()->last_point().distance_to_square(start_near)) {
                    result.emplace_back(*m_entities.front(), true);
                } else {
                    result.emplace_back(*m_entities.front(), false);
                }
            }
            if (m_entities.size() > 1) {
                if (m_entities.back()->is_collection()) {
                    assert(dynamic_cast<ExtrusionEntityCollection *>(m_entities.front()) != nullptr);
                    static_cast<ExtrusionEntityCollection *>(m_entities.back())->chained_path_from(start_near);
                } else if (m_entities.back()->can_reverse() &&
                           m_entities.back()->first_point().distance_to_square(start_near) >
                               m_entities.back()->last_point().distance_to_square(start_near)) {
                    result.emplace_back(*m_entities.back(), true);
                } else {
                    result.emplace_back(*m_entities.back(), false);
                }
                // can't sort myself, ask first and last thing to sort itself so the first point of each are the best ones

                // now check if it's better for us to reverse
                Point first_point = result.front().flipped() ? result.front().extrusion_entity().last_point() :
                                                               result.front().extrusion_entity().first_point();
                Point last_point = result.back().flipped() ? result.back().extrusion_entity().first_point() :
                                                             result.back().extrusion_entity().last_point();
                if (start_near.distance_to_square(first_point) > start_near.distance_to_square(last_point)) {
                    // switch entities
                    need_reverse = true;
                    // this->reverse();
                }
            } else {
                // only one child (useless collection)
                need_reverse = result.front().flipped();
            }
            result.clear();
        }
        // now we are in our good order, update the internals to the final order
        for (ExtrusionEntity *entity : m_entities) {
            result.emplace_back(*entity, need_reverse);
        }
        if (need_reverse) {
            std::reverse(result.begin(), result.end());
        }
        return result;
    } else {
        return chain_extrusion_references(this->m_entities, &start_near);
    }
}

void ExtrusionEntityCollection::polygons_covered_by_width(Polygons &out, const float scaled_epsilon) const
{
    for (const ExtrusionEntity *entity : this->entities())
        entity->polygons_covered_by_width(out, scaled_epsilon);
}

void ExtrusionEntityCollection::polygons_covered_by_spacing(Polygons &out, const float spacing_ratio, const float scaled_epsilon) const
{
    for (const ExtrusionEntity *entity : this->entities())
        entity->polygons_covered_by_spacing(out, spacing_ratio, scaled_epsilon);
}

// Recursively count paths and loops contained in this collection.
size_t ExtrusionEntityCollection::items_count() const
{
    return CountEntities().count(*this);
}

void
CountEntities::use(const ExtrusionEntityCollection &coll) {
    for (const ExtrusionEntity* entity : coll.entities()) {
        entity->visit(*this);
    }
}

// Returns a single vector of pointers to all non-collection items contained in this one.
ExtrusionEntityCollection ExtrusionEntityCollection::flatten(bool preserve_ordering) const
{
    //ExtrusionEntityCollection coll;
    //this->flatten(&coll, preserve_ordering);
    //return coll;
    return FlatenEntities(preserve_ordering).flatten(*this);

}

void ExtrusionEntityCollection::flatten(bool preserve_ordering, ExtrusionEntityCollection& out) const
{
    if (!this->can_sort()  && preserve_ordering) {
        out.append(this->flatten(preserve_ordering));
    }else{
        FlatenEntities flattener(preserve_ordering);
        flattener.use(*this);
        //tranfert owner of entities.
        out.m_entities.insert(out.m_entities.begin(), flattener.get().entities().begin(), flattener.get().entities().end());
        flattener.set().m_entities.clear();
    }
}

void FlatenEntities::use(const ExtrusionEntityCollection &coll) {
    if ((!coll.can_sort() || !this->to_fill.can_sort()) && preserve_ordering) {
        FlatenEntities unsortable(coll, preserve_ordering);
        for (const ExtrusionEntity* entity : coll.entities()) {
            entity->visit(unsortable);
        }
        to_fill.append(std::move(unsortable.to_fill));
    } else {
        for (const ExtrusionEntity* entity : coll.entities()) {
            entity->visit(*this);
        }
    }
}

ExtrusionEntityCollection&& FlatenEntities::flatten(const ExtrusionEntityCollection &to_flatten) && {
    use(to_flatten);
    return std::move(to_fill);
}

}
