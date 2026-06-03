///|/ Copyright (c) Prusa Research 2016 - 2023 Vojtěch Bubník @bubnikv, Lukáš Hejl @hejllukas, Filip Sykala @Jony01, Lukáš Matěna @lukasmatena
///|/ Copyright (c) SuperSlicer 2023 Remi Durand @supermerill
///|/ Copyright (c) Slic3r 2013 - 2016 Alessandro Ranellucci @alranel
///|/
///|/ PrusaSlicer is released under the terms of the AGPLv3 or higher
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/
#ifndef slic3r_ExtrusionEntityCollection_hpp_
#define slic3r_ExtrusionEntityCollection_hpp_

#include "Exception.hpp"
#include "ExtrusionEntity.hpp"
#include "libslic3r.h"

namespace Slic3r {

#if 0
// Remove those items from extrusion_entities, that do not match role.
// Do nothing if role is mixed.
// Removed elements are NOT being deleted.
void filter_by_extrusion_role_in_place(ExtrusionEntitiesPtr &extrusion_entities, ExtrusionRole role);

// Return new vector of ExtrusionEntities* with only those items from input extrusion_entities, that match role.
// Return all extrusion entities if role is mixed.
// Returned extrusion entities are shared with the source vector, they are NOT cloned, they are considered to be owned by extrusion_entities.
inline ExtrusionEntitiesPtr filter_by_extrusion_role(const ExtrusionEntitiesPtr &extrusion_entities, ExtrusionRole role)
{
	ExtrusionEntitiesPtr out { extrusion_entities }; 
	filter_by_extrusion_role_in_place(out, role);
	return out;
}
#endif

class ExtrusionEntityCollection : public ExtrusionEntity
{
private:
    mutable ExtrusionEntitiesPtr m_entities_cache;

    void rebuild_entities_cache() const;

public:
    virtual ExtrusionEntityCollection* clone() const override { return new ExtrusionEntityCollection(*this); }
    // Create a new object, initialize it with this object using the move semantics.
	virtual ExtrusionEntityCollection* clone_move() override { return new ExtrusionEntityCollection(std::move(*this)); }


    /// Owned ExtrusionEntities and descendent ExtrusionEntityCollections.
    /// Iterating over this needs to check each child to see if it, too is a collection.
    /// FIXME Warning: not a true const, the entities inside can be modified, and if the entities are deleted -> crash
    const ExtrusionEntitiesPtr& entities() const { this->rebuild_entities_cache(); return m_entities_cache; }
    ExtrusionEntityCollection() : ExtrusionEntity(ExtrusionEntity::Children(), true, true, false) {}
    ExtrusionEntityCollection(bool can_sort, bool can_reverse) : ExtrusionEntity(ExtrusionEntity::Children(), can_sort, can_reverse, false) {}
    ExtrusionEntityCollection(const ExtrusionEntityCollection &other) : ExtrusionEntity(other) {}
    ExtrusionEntityCollection(ExtrusionEntityCollection &&other) : ExtrusionEntity(std::move(other)) {}
    explicit ExtrusionEntityCollection(const ExtrusionPaths &paths);
    ExtrusionEntityCollection& operator=(const ExtrusionEntityCollection &other);
    ExtrusionEntityCollection& operator=(ExtrusionEntityCollection &&other);
    ~ExtrusionEntityCollection() override { clear(); }
    // move all entitites from src into this
    void append_move_from(ExtrusionEntityCollection &src);

    /// Operator to convert and flatten this collection to a single vector of ExtrusionPaths.
    explicit operator ExtrusionPaths() const;

    ExtrusionEntitiesPtr::const_iterator    cbegin() const { return this->entities().cbegin(); }
    ExtrusionEntitiesPtr::const_iterator    cend()   const { return this->entities().cend(); }
    ExtrusionEntitiesPtr::const_iterator    begin()  const { return this->entities().cbegin(); }
    ExtrusionEntitiesPtr::const_iterator    end()    const { return this->entities().cend(); }
    //ExtrusionEntitiesPtr::iterator          begin()        { return this->entities.begin(); }
    //ExtrusionEntitiesPtr::iterator          end()          { return this->entities.end(); }

    bool is_collection() const override { return ExtrusionEntity::is_collection(); }
    ExtrusionRole role() const override;
    bool has_role(ExtrusionRole test_role) const override;
    void set_can_sort_reverse(bool can_sort, bool can_reverse) { ExtrusionEntity::set_can_sort_reverse(can_sort, can_reverse); }
    bool can_sort() const { return ExtrusionEntity::can_sort(); }
    bool can_reverse() const override { return can_sort() || this->m_can_reverse; }
    void clear();
    void swap (ExtrusionEntityCollection &c);
    void append(const ExtrusionEntity &entity) { this->append_child(ExtrusionEntityUPtr(entity.clone())); m_entities_cache.clear(); }
    void append(ExtrusionEntity &&entity) { this->append_child(ExtrusionEntityUPtr(entity.clone_move())); m_entities_cache.clear(); }
    void append(ExtrusionEntityUPtr &&entity) { this->append_child(std::move(entity)); m_entities_cache.clear(); }
    // take ownership, empty the container.
    template<typename ENTITY> void append(std::unique_ptr<ENTITY> &entity)
    {
        static_assert(std::is_base_of<ExtrusionEntity, ENTITY>::value, "ENTITY not derived from ExtrusionEntity in ExtrusionCollection::append(unique_ptr<ENTITY>)");
        this->append_child(std::move(entity));
        m_entities_cache.clear();
    }
    template<typename ENTITY> void append(std::unique_ptr<ENTITY> &&entity)
    {
        static_assert(std::is_base_of<ExtrusionEntity, ENTITY>::value, "ENTITY not derived from ExtrusionCollection::append(unique_ptr<ENTITY>)");
        this->append_child(std::move(entity));
        m_entities_cache.clear();
    }
    void append_at(ExtrusionEntity &&entity, size_t position) { assert(position <= this->child_count()); this->insert_child(position, ExtrusionEntityUPtr(entity.clone_move())); m_entities_cache.clear(); }
    void append(const ExtrusionEntitiesPtr &entities) { 
        this->children().reserve(this->children().size() + entities.size());
        for (const ExtrusionEntity *ptr : entities)
            this->append_child(ExtrusionEntityUPtr(ptr->clone()));
        m_entities_cache.clear();
    }
    void append(ExtrusionEntitiesPtr &&src) {
        this->children().reserve(this->children().size() + src.size());
        for (ExtrusionEntity *ptr : src)
            this->append_child(ExtrusionEntityUPtr(ptr));
        src.clear();
        m_entities_cache.clear();
    }
    void append(const ExtrusionPaths &paths) {
        this->children().reserve(this->children().size() + paths.size());
        for (const ExtrusionPath &path : paths)
            this->append_child(ExtrusionEntityUPtr(path.clone()));
        m_entities_cache.clear();
    }
    void append(ExtrusionPaths &&paths) {
        this->children().reserve(this->children().size() + paths.size());
        for (ExtrusionPath &path : paths)
            this->append_child(std::make_unique<ExtrusionPath>(std::move(path)));
        m_entities_cache.clear();
    }
    ExtrusionEntityUPtr release(size_t i);
    ExtrusionEntityUPtr release_back();
    void erase(size_t begin, size_t end);
    void replace(size_t i, const ExtrusionEntity &entity);
    void remove(size_t i);
    ExtrusionEntityReferences chained_path_from(const Point &start_near);
    void reverse() override;
    const Point& first_point() const override { return this->entities().front()->first_point(); }
    const Point& last_point() const override { return this->entities().back()->last_point(); }
    const Point& middle_point() const override { return this->entities()[this->entities().size() / 2]->middle_point(); }
    // Produce a list of 2D polygons covered by the extruded paths, offsetted by the extrusion width.
    // Increase the offset by scaled_epsilon to achieve an overlap, so a union will produce no gaps.
    void polygons_covered_by_width(Polygons &out, const float scaled_epsilon) const override;
    // Produce a list of 2D polygons covered by the extruded paths, offsetted by the extrusion spacing.
    // Increase the offset by scaled_epsilon to achieve an overlap, so a union will produce no gaps.
    // Useful to calculate area of an infill, which has been really filled in by a 100% rectilinear infill.
    void polygons_covered_by_spacing(Polygons &out, const float spacing_ratio, const float scaled_epsilon) const override;
    Polygons polygons_covered_by_width(const float scaled_epsilon = 0.f) const
        { Polygons out; this->polygons_covered_by_width(out, scaled_epsilon); return out; }
    Polygons polygons_covered_by_spacing(const float spacing_ratio, const float scaled_epsilon) const
        { Polygons out; this->polygons_covered_by_spacing(out, spacing_ratio, scaled_epsilon); return out; }

    /// count of entities inside this container.
    size_t size() const { return entities().size(); }
    // Recursively count paths and loops contained in this collection. 
    // this->items_count() >= this->size()
    size_t items_count() const;
    /// Deprecated compatibility helper. Flattening clones/rebuilds the
    /// extrusion tree, so properties carried by intermediate nodes may stop
    /// being visible to the flattened children. New ordering code should walk
    /// the original tree and choose candidates without destroying hierarchy.
    [[deprecated("flatten() rebuilds extrusion hierarchy and may lose inherited node properties; use a tree visitor for ordering instead.")]]
    ExtrusionEntityCollection flatten(bool preserve_ordering) const;
    [[deprecated("flatten() rebuilds extrusion hierarchy and may lose inherited node properties; use a tree visitor for ordering instead.")]]
    void flatten(bool preserve_ordering, ExtrusionEntityCollection& out) const;
    double total_volume() const override { double volume=0.; for (const auto& ent : entities()) volume+=ent->total_volume(); return volume; }

    // Following methods shall never be called on an ExtrusionEntityCollection.
    ArcPolyline as_polyline() const override {
        throw Slic3r::RuntimeError("Calling as_polyline() on a ExtrusionEntityCollection");
        return ArcPolyline();
    };

    void collect_polylines(ArcPolylines &dst) const override {
        for (const ExtrusionEntity *extrusion_entity : this->entities())
            extrusion_entity->collect_polylines(dst);
    }

    void   collect_points(Points &dst) const override {
        for (const ExtrusionEntity *extrusion_entity : this->entities())
            extrusion_entity->collect_points(dst);
    }

    coordf_t length() const override {
        throw Slic3r::RuntimeError("Calling length() on a ExtrusionEntityCollection");
        return 0.;        
    }
    bool empty() const override {
        for (const ExtrusionEntity *extrusion_entity : this->entities())
            if (!extrusion_entity->empty())
                return false;
        return true;
    }
    using ExtrusionEntity::visit;
    virtual void visit(ExtrusionVisitor &visitor) override;
    virtual void visit(ExtrusionVisitorConst &visitor) const override;
};

inline void extrusion_entities_append_paths(ExtrusionEntityCollection &dst, Polylines &polylines, ExtrusionRole role, double mm3_per_mm, float width, float height, bool can_reverse = true)
{
    //dst.reserve(dst.size() + polylines.size());
    for (Polyline &polyline : polylines)
        if (polyline.is_valid()) {
            if (polyline.back() == polyline.front()) {
                ExtrusionPath path(ExtrusionAttributes(role, ExtrusionFlow(mm3_per_mm, width, height)), nullptr, can_reverse);
                path.polyline() = polyline;
                dst.append(ExtrusionLoop(std::move(path)));
            } else {
                ExtrusionPath extrusion_path(ExtrusionAttributes(role, ExtrusionFlow(mm3_per_mm, width, height)), nullptr, can_reverse);
                extrusion_path.polyline() = polyline;
                dst.append(std::move(extrusion_path));
            }
        }
}

inline void extrusion_entities_append_paths(ExtrusionEntityCollection &dst, Polylines &&polylines, ExtrusionAttributes &&path_attr, bool can_reverse = true)
{
    //dst.reserve(dst.size() + polylines.size());
    for (Polyline &polyline : polylines)
        if (polyline.is_valid()) {
            if (polyline.back() == polyline.front()) {
                ExtrusionPath path(path_attr, nullptr, can_reverse);
                path.polyline() = polyline;
                dst.append(ExtrusionLoop(std::move(path)));
            } else {
                ExtrusionPath extrusion_path(path_attr, nullptr, can_reverse);
                extrusion_path.polyline() = std::move(polyline);
                dst.append(std::move(extrusion_path));
            }
        }
    polylines.clear();
}

inline void extrusion_entities_append_loops(ExtrusionEntityCollection &dst, Polygons &loops, ExtrusionAttributes &&path_attr, bool can_reverse = true) {
    //dst.reserve(dst.size() + loops.size());
    for (Polygon & polygon : loops) {
        if (polygon.is_valid()) {
            ExtrusionPath path(path_attr, nullptr, can_reverse);
            path.polyline().append(polygon.points);
            path.polyline().append(path.polyline().front());
            dst.append(ExtrusionLoop(std::move(path)));
        }
    }
}

inline void extrusion_entities_append_loops(ExtrusionEntityCollection &dst, Polygons &&loops, ExtrusionAttributes &&path_attr, bool can_reverse = true)
{
    //dst.reserve(dst.size() + loops.size());
    for (Polygon &polygon : loops) {
        if (polygon.is_valid()) {
            ExtrusionPath path(path_attr, nullptr, can_reverse);
            path.polyline().append(std::move(polygon.points));
            path.polyline().append(path.polyline().front());
            ExtrusionLoop loop(std::move(path));
            //default to ccw
            if (loop.is_clockwise()) loop.reverse(); //loop.make_counter_clockwise();
            dst.append(std::move(loop));
        }
    }
    loops.clear();
}

inline void extrusion_entities_append_loops_and_paths(ExtrusionEntityCollection &dst, Polylines &&polylines, ExtrusionAttributes &&path_attr, bool can_reverse = true)
{
    //dst.reserve(dst.size() + polylines.size());
    for (Polyline &polyline : polylines) {
        if (polyline.is_valid()) {
            if (polyline.is_closed()) {
                ExtrusionPath extrusion_path(path_attr, nullptr, can_reverse);
                extrusion_path.polyline() = std::move(polyline);
                dst.append(ExtrusionLoop(std::move(extrusion_path)));
            } else {
                ExtrusionPath extrusion_path(path_attr, nullptr, can_reverse);
                extrusion_path.polyline() = std::move(polyline);
                dst.append(std::move(extrusion_path));
            }
        }
    }
    polylines.clear();
}

} // namespace Slic3r

#endif
