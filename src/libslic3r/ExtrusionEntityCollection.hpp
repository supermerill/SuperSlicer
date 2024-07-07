#ifndef slic3r_ExtrusionEntityCollection_hpp_
#define slic3r_ExtrusionEntityCollection_hpp_

#include "libslic3r.h"
#include "Exception.hpp"
#include "ExtrusionEntity.hpp"

namespace Slic3r {

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

class ExtrusionEntityCollection : public ExtrusionEntity
{
private:
    // set to tru to forbit to reorder and reverse all entities indie us.
    bool m_no_sort;
    ExtrusionEntitiesPtr m_entities;     // we own these entities
public:
    virtual ExtrusionEntityCollection* clone() const override { return new ExtrusionEntityCollection(*this); }
    // Create a new object, initialize it with this object using the move semantics.
    virtual ExtrusionEntityCollection* clone_move() override { return new ExtrusionEntityCollection(std::move(*this)); }


    /// Owned ExtrusionEntities and descendent ExtrusionEntityCollections.
    /// Iterating over this needs to check each child to see if it, too is a collection.
    /// FIXME Warning: not a true const, the entities inside can be modified, and if the entities are deleted -> crash
    const ExtrusionEntitiesPtr& entities() const { return m_entities; }
    ExtrusionEntitiesPtr& set_entities() { return m_entities; }
    ExtrusionEntityCollection() : m_no_sort(false), ExtrusionEntity(true) {}
    ExtrusionEntityCollection(bool can_sort, bool can_reverse) : m_no_sort(!can_sort), ExtrusionEntity(can_reverse) {}
    ExtrusionEntityCollection(const ExtrusionEntityCollection &other) : m_no_sort(other.m_no_sort), ExtrusionEntity(other.m_can_reverse) { this->append(other.entities()); }
    ExtrusionEntityCollection(ExtrusionEntityCollection &&other) : m_entities(std::move(other.m_entities)), m_no_sort(other.m_no_sort), ExtrusionEntity(other.m_can_reverse) {}
    explicit ExtrusionEntityCollection(const ExtrusionPaths &paths);
    ExtrusionEntityCollection& operator=(const ExtrusionEntityCollection &other);
    ExtrusionEntityCollection& operator=(ExtrusionEntityCollection &&other) {
        this->clear();
        this->m_entities = std::move(other.m_entities);
        this->m_no_sort  = other.m_no_sort;
        this->m_can_reverse = other.m_can_reverse;
        return *this;
    }
    ~ExtrusionEntityCollection() override { clear(); }
    // move all entitites from src into this
    void append_move_from(ExtrusionEntityCollection &src) {
        this->append(std::move(src.m_entities));
        src.m_entities = {};
    }

    /// Operator to convert and flatten this collection to a single vector of ExtrusionPaths.
    explicit operator ExtrusionPaths() const;

    bool is_collection() const override { return true; }
    ExtrusionRole role() const override {
        ExtrusionRole out = erNone;
        for (const ExtrusionEntity *ee : m_entities) {
            ExtrusionRole er = ee->role();
            if (out == erNone) {
                out = er;
            }else if (out != er) {
                return erMixed;
            }
        }
        return out;
    }
    void set_can_sort_reverse(bool can_sort, bool can_reverse) { this->m_no_sort = !can_sort; this->m_can_reverse = can_reverse; }
    bool can_sort() const { return !this->m_no_sort; }
    bool can_reverse() const override { return can_sort() || this->m_can_reverse; }
    bool empty() const { return this->m_entities.empty(); }
    void clear();
    void swap (ExtrusionEntityCollection &c);
    void append(const ExtrusionEntity &entity) { this->m_entities.emplace_back(entity.clone()); }
    void append(ExtrusionEntity &&entity) { this->m_entities.emplace_back(entity.clone_move()); }
    void append(const ExtrusionEntitiesPtr &entities) {
        this->m_entities.reserve(this->m_entities.size() + entities.size());
        for (const ExtrusionEntity *ptr : entities)
            this->m_entities.emplace_back(ptr->clone());
    }
    void append(ExtrusionEntitiesPtr &&src) {
        if (m_entities.empty())
            m_entities = std::move(src);
        else {
            std::move(std::begin(src), std::end(src), std::back_inserter(m_entities));
            src.clear();
        }
    }
    void append(const ExtrusionPaths &paths) {
        this->m_entities.reserve(this->m_entities.size() + paths.size());
        for (const ExtrusionPath &path : paths)
            this->m_entities.emplace_back(path.clone());
    }
    void append(ExtrusionPaths &&paths) {
        this->m_entities.reserve(this->m_entities.size() + paths.size());
        for (ExtrusionPath &path : paths)
            this->m_entities.emplace_back(new ExtrusionPath(std::move(path)));
    }
    void replace(size_t i, const ExtrusionEntity &entity);
    void remove(size_t i);
    void chained_path_from(const Point &start_near);
    void reverse() override;
    const Point& first_point() const override { return this->entities().front()->first_point(); }
    const Point& last_point() const override { return this->entities().back()->last_point(); }
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

    /// Recursively count paths and loops contained in this collection
    size_t items_count() const;
    /// Returns a flattened copy of this ExtrusionEntityCollection. That is, all of the items in its entities() vector are not collections.
    /// You should be iterating over flatten().entities() if you are interested in the underlying ExtrusionEntities (and don't care about hierarchy).
    /// \param preserve_ordering Flag to method that will flatten if and only if the underlying collection is sortable when True (default: False).
    ExtrusionEntityCollection flatten(bool preserve_ordering = false) const;
    double total_volume() const override { double volume=0.; for (const auto& ent : entities()) volume+=ent->total_volume(); return volume; }

    // Following methods shall never be called on an ExtrusionEntityCollection.
    PolylineOrArc as_polyline() const override {
        throw Slic3r::RuntimeError("Calling as_polyline() on a ExtrusionEntityCollection");
        return PolylineOrArc();
    };

    void collect_polylines(PolylinesOrArcs &dst) const override {
        for (const ExtrusionEntity* extrusion_entity : this->entities())
            extrusion_entity->collect_polylines(dst);
    }

    void   collect_points(Points &dst) const override {
        for (ExtrusionEntity* extrusion_entity : this->entities())
            extrusion_entity->collect_points(dst);
    }

    double length() const override {
        throw Slic3r::RuntimeError("Calling length() on a ExtrusionEntityCollection");
        return 0.;
    }
    virtual void visit(ExtrusionVisitor &visitor) override { visitor.use(*this); };
    virtual void visit(ExtrusionVisitorConst &visitor) const override{ visitor.use(*this); };
};

//// visitors /////

class CountEntities : public ExtrusionVisitorConst {
public:
    size_t count(const ExtrusionEntity &coll) { coll.visit(*this); return leaf_number; }
    size_t leaf_number = 0;
    virtual void default_use(const ExtrusionEntity &entity) override { ++leaf_number; }
    virtual void use(const ExtrusionEntityCollection &coll) override;
};

class FlatenEntities : public ExtrusionVisitorConst {
    ExtrusionEntityCollection to_fill;
    bool preserve_ordering;
public:
    FlatenEntities(bool preserve_ordering) : preserve_ordering(preserve_ordering) {}
    FlatenEntities(ExtrusionEntityCollection pattern, bool preserve_ordering) : preserve_ordering(preserve_ordering) {
        to_fill.set_can_sort_reverse(pattern.can_sort(), pattern.can_reverse());
    }
    ExtrusionEntityCollection get() {
        return to_fill;
    };
    ExtrusionEntityCollection&& flatten(const ExtrusionEntityCollection &to_flatten) &&;
    virtual void default_use(const ExtrusionEntity &entity) override { to_fill.append(entity); }
    virtual void use(const ExtrusionEntityCollection &coll) override;
};

inline void extrusion_entities_append_paths(ExtrusionEntityCollection &dst, Polylines &polylines, ExtrusionRole role, double mm3_per_mm, float width, float height, bool can_reverse = true)
{
    //dst.reserve(dst.size() + polylines.size());
    for (Polyline &polyline : polylines)
        if (polyline.is_valid()) {
            if (polyline.back() == polyline.front()) {
                ExtrusionPath path(role, mm3_per_mm, width, height, can_reverse);
                path.polyline = polyline;
                dst.append(ExtrusionLoop(std::move(path)));
            } else {
                ExtrusionPath extrusion_path(role, mm3_per_mm, width, height, can_reverse);
                extrusion_path.polyline = polyline;
                dst.append(std::move(extrusion_path));
            }
        }
}

inline void extrusion_entities_append_paths(ExtrusionEntityCollection &dst, Polylines &&polylines, ExtrusionRole role, double mm3_per_mm, float width, float height, bool can_reverse = true)
{
    //dst.reserve(dst.size() + polylines.size());
    for (Polyline &polyline : polylines)
        if (polyline.is_valid()) {
            if (polyline.back() == polyline.front()) {
                ExtrusionPath path(role, mm3_per_mm, width, height, can_reverse);
                path.polyline = polyline;
                dst.append(ExtrusionLoop(std::move(path)));
            } else {
                ExtrusionPath extrusion_path(role, mm3_per_mm, width, height, can_reverse);
                extrusion_path.polyline = std::move(polyline);
                dst.append(std::move(extrusion_path));
            }
        }
    polylines.clear();
}

inline void extrusion_entities_append_loops(ExtrusionEntityCollection &dst, Polygons &loops, ExtrusionRole role, double mm3_per_mm, float width, float height, bool can_reverse = true) {
    //dst.reserve(dst.size() + loops.size());
    for (Polygon & polygon : loops) {
        if (polygon.is_valid()) {
            ExtrusionPath path(role, mm3_per_mm, width, height, can_reverse);
            path.polyline.append(polygon.points);
            path.polyline.append(path.polyline.front());
            dst.append(ExtrusionLoop(std::move(path)));
        }
    }
}

inline void extrusion_entities_append_loops(ExtrusionEntityCollection &dst, Polygons &&loops, ExtrusionRole role, double mm3_per_mm, float width, float height, bool can_reverse = true)
{
    //dst.reserve(dst.size() + loops.size());
    for (Polygon &polygon : loops) {
        if (polygon.is_valid()) {
            ExtrusionPath path(role, mm3_per_mm, width, height, can_reverse);
            path.polyline.append(std::move(polygon.points));
            path.polyline.append(path.polyline.front());
            ExtrusionLoop loop(std::move(path));
            //default to ccw
            loop.make_counter_clockwise();
            dst.append(std::move(loop));
        }
    }
    loops.clear();
}

inline void extrusion_entities_append_loops_and_paths(ExtrusionEntityCollection &dst, Polylines &&polylines, ExtrusionRole role, double mm3_per_mm, float width, float height, bool can_reverse = true)
{
    //dst.reserve(dst.size() + polylines.size());
    for (Polyline &polyline : polylines) {
        if (polyline.is_valid()) {
            if (polyline.is_closed()) {
                ExtrusionPath extrusion_path(role, mm3_per_mm, width, height, can_reverse);
                extrusion_path.polyline = std::move(polyline);
                dst.append(ExtrusionLoop(std::move(extrusion_path)));
            } else {
                ExtrusionPath extrusion_path(role, mm3_per_mm, width, height, can_reverse);
                extrusion_path.polyline = std::move(polyline);
                dst.append(std::move(extrusion_path));
            }
        }
    }
    polylines.clear();
}

#ifdef _DEBUG
class TestCollection : public ExtrusionVisitorRecursiveConst {
public:
    virtual void default_use(const ExtrusionEntity& entity) override {
        assert(entity.as_polyline().size() > 0);
    }
    virtual void use(const ExtrusionEntityCollection& coll) override {
        for (const ExtrusionEntity* entity : coll.entities()) {
            assert(entity);
            std::cout << "entity at " << ((uint64_t)(void*)entity) << "\n";
            entity->visit(*this);
        }

    }
};
#endif

} // namespace Slic3r

#endif
