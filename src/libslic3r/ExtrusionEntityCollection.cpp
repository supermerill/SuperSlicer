///|/ Copyright (c) Prusa Research 2016 - 2023 Vojtěch Bubník @bubnikv, Lukáš Hejl @hejllukas, Lukáš Matěna @lukasmatena
///|/ Copyright (c) SuperSlicer 2023 Remi Durand @supermerill
///|/ Copyright (c) Slic3r 2013 - 2016 Alessandro Ranellucci @alranel
///|/ Copyright (c) 2015 Maksim Derbasov @ntfshard
///|/ Copyright (c) 2014 Petr Ledvina @ledvinap
///|/
///|/ PrusaSlicer is released under the terms of the AGPLv3 or higher
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/
#include "ExtrusionEntityCollection.hpp"

#include <algorithm>

#include "ExtrusionEntityVisitors.hpp"
#include "ShortestPath.hpp"

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
    : ExtrusionEntity(ExtrusionEntity::Children(), true, true, false)
{
    this->append(paths);
}

void ExtrusionEntityCollection::rebuild_entities_cache() const
{
    assert(!this->is_leaf());
    const Children &children = ExtrusionEntity::children();
    if (m_entities_cache.size() == children.size()) {
        bool valid = true;
        for (size_t idx = 0; idx < children.size(); ++idx) {
            if (m_entities_cache[idx] != children[idx].get()) {
                valid = false;
                break;
            }
        }
        if (valid)
            return;
    }

    m_entities_cache.clear();
    m_entities_cache.reserve(children.size());
    for (const ExtrusionEntityUPtr &entity : children)
        m_entities_cache.emplace_back(entity.get());
}

ExtrusionEntityCollection& ExtrusionEntityCollection::operator= (const ExtrusionEntityCollection &other)
{
    if (this != &other) {
        this->clear();
        ExtrusionEntity::operator=(other);
    }
    return *this;
}

ExtrusionEntityCollection& ExtrusionEntityCollection::operator=(ExtrusionEntityCollection &&other)
{
    if (this != &other) {
        this->clear();
        ExtrusionEntity::operator=(std::move(other));
    }
    return *this;
}

void ExtrusionEntityCollection::append_move_from(ExtrusionEntityCollection &src)
{
    Children &dst = this->children();
    Children &src_children = src.children();
    dst.reserve(dst.size() + src_children.size());
    dst.insert(dst.end(), std::make_move_iterator(src_children.begin()), std::make_move_iterator(src_children.end()));
    src_children.clear();
}

void ExtrusionEntityCollection::swap(ExtrusionEntityCollection &c)
{
    std::swap(this->m_content, c.m_content);
    std::swap(this->m_can_sort, c.m_can_sort);
    std::swap(this->m_can_reverse, c.m_can_reverse);
    std::swap(this->m_id, c.m_id);
    this->m_entities_cache.clear();
    c.m_entities_cache.clear();
}

ExtrusionRole ExtrusionEntityCollection::role() const
{
    ExtrusionRole out{ ExtrusionRole::None };
    for (const ExtrusionEntity *ee : this->entities()) {
        ExtrusionRole er = ee->role();
        if (out == ExtrusionRole::None) {
            out = er;
        }else if (out != er) {
            return ExtrusionRole::Mixed;
        }
    }
    return out;
}

bool ExtrusionEntityCollection::has_role(ExtrusionRole test_role) const
{
    if (this->entities().empty())
        return false;
    for (const ExtrusionEntity *entity : this->entities())
        if (entity->has_role(test_role)) {
            return true;
        }
    return false;
}

void ExtrusionEntityCollection::clear()
{
    m_entities_cache.clear();
    ExtrusionEntity::children().clear();
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
    for (ExtrusionEntityUPtr &ptr : this->children())
    {
        // Don't reverse it if it's a loop, as it doesn't change anything in terms of elements ordering
        // and caller might rely on winding order
        if (ptr->can_reverse() && !ptr->is_loop())
            ptr->reverse();
    }
    std::reverse(this->children().begin(), this->children().end());
    this->m_entities_cache.clear();
}

void ExtrusionEntityCollection::replace(size_t i, const ExtrusionEntity &entity)
{
    this->children()[i] = ExtrusionEntityUPtr(entity.clone());
    this->m_entities_cache.clear();
}

void ExtrusionEntityCollection::remove(size_t i)
{
    this->children().erase(this->children().begin() + i);
    this->m_entities_cache.clear();
}

ExtrusionEntityUPtr ExtrusionEntityCollection::release(size_t i)
{
    Children &children = this->children();
    assert(i < children.size());
    ExtrusionEntityUPtr out = std::move(children[i]);
    children.erase(children.begin() + i);
    this->m_entities_cache.clear();
    return out;
}

ExtrusionEntityUPtr ExtrusionEntityCollection::release_back()
{
    Children &children = this->children();
    assert(!children.empty());
    ExtrusionEntityUPtr out = std::move(children.back());
    children.pop_back();
    this->m_entities_cache.clear();
    return out;
}

void ExtrusionEntityCollection::erase(size_t begin, size_t end)
{
    Children &children = this->children();
    assert(begin <= end);
    assert(end <= children.size());
    children.erase(children.begin() + begin, children.begin() + end);
    this->m_entities_cache.clear();
}

// note: chained_path_from only this collection. You still need to chained_path_from the child collections.
ExtrusionEntityReferences ExtrusionEntityCollection::chained_path_from(const Point &start_near)
{
    if (!this->can_sort()) {
        ExtrusionEntityReferences result{};
        bool need_reverse = false;
        if (this->m_can_reverse) {
            Children &children = this->children();
            if (!children.empty()) {
                if (children.front()->is_collection()) {
                    assert(dynamic_cast<ExtrusionEntityCollection *>(children.front().get()) != nullptr);
                    ExtrusionEntityCollection *front_coll = static_cast<ExtrusionEntityCollection *>(
                        children.front().get());
                    result = front_coll->chained_path_from(start_near);
                    assert(!result.empty());
                } else if (children.front()->can_reverse() &&
                           children.front()->first_point().distance_to_square(start_near) >
                               children.front()->last_point().distance_to_square(start_near)) {
                    result.emplace_back(*children.front(), true);
                } else {
                    result.emplace_back(*children.front(), false);
                }
            }
            if (children.size() > 1) {
                if (children.back()->is_collection()) {
                    assert(dynamic_cast<ExtrusionEntityCollection *>(children.front().get()) != nullptr);
                    static_cast<ExtrusionEntityCollection *>(children.back().get())->chained_path_from(start_near);
                } else if (children.back()->can_reverse() &&
                           children.back()->first_point().distance_to_square(start_near) >
                               children.back()->last_point().distance_to_square(start_near)) {
                    result.emplace_back(*children.back(), true);
                } else {
                    result.emplace_back(*children.back(), false);
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
        for (ExtrusionEntityUPtr &entity : this->children()) {
            result.emplace_back(*entity, need_reverse);
        }
        if (need_reverse) {
            std::reverse(result.begin(), result.end());
        }
        return result;
    } else {
        return chain_extrusion_references(this->entities(), &start_near);
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

void ExtrusionEntityCollection::visit(ExtrusionVisitor &visitor)
{
    visitor.use(*this);
}

void ExtrusionEntityCollection::visit(ExtrusionVisitorConst &visitor) const
{
    visitor.use(*this);
}

// Recursively count paths and loops contained in this collection.
size_t ExtrusionEntityCollection::items_count() const
{
    return CountEntities().count(*this);
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
    if (!this->can_sort() && preserve_ordering && this->entities().size() > 1) {
        out.append(this->flatten(preserve_ordering));
    } else {
        FlatenEntities flattener(preserve_ordering);
        flattener.traverse(*this);
        //tranfert owner of entities.
        ExtrusionEntityCollection &flat = flattener.set();
        Children &out_children = out.children();
        Children &flat_children = flat.children();
        out_children.insert(out_children.begin(), std::make_move_iterator(flat_children.begin()), std::make_move_iterator(flat_children.end()));
        flat_children.clear();
        out.m_entities_cache.clear();
    }
}

}
