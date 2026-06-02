///|/ Copyright (c) Prusa Research 2016 - 2023 Vojtěch Bubník @bubnikv, Lukáš Hejl @hejllukas, Lukáš Matěna @lukasmatena, Enrico Turri @enricoturri1966, Oleksandra Iushchenko @YuSanka
///|/ Copyright (c) SuperSlicer 2023 Remi Durand @supermerill
///|/ Copyright (c) 2017 Eyal Soha @eyal0
///|/ Copyright (c) Slic3r 2013 - 2016 Alessandro Ranellucci @alranel
///|/
///|/ PrusaSlicer is released under the terms of the AGPLv3 or higher
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/
#ifndef slic3r_ExtrusionEntity_hpp_
#define slic3r_ExtrusionEntity_hpp_

#include <atomic>
#include <cassert>
#include <iterator>
#include <limits>
#include <memory>
#include <numeric>
#include <optional>
#include <string_view>
#include <type_traits>
#include <variant>
#include <vector>

#include "ExtrusionRole.hpp"
#include "ExtrusionProperty.hpp"
#include "libslic3r.h"
#include "Polygon.hpp"
#include "Polyline.hpp"

namespace Slic3r {

class ExPolygon;
using ExPolygons = std::vector<ExPolygon>;
class ExtrusionEntityCollection;
class Extruder;


class ExtrusionEntity;
class ExtrusionPath;
class ExtrusionMultiPath;
class ExtrusionLoop;
class ExtrusionNop;
class ExtrusionVisitor;
class ExtrusionVisitorConst;

using ExtrusionEntityUPtr = std::unique_ptr<ExtrusionEntity>;
using ExtrusionEntityUPtrs = std::vector<ExtrusionEntityUPtr>;

class ExtrusionEntity : public ExtrusionPropertyContainer
{
public:
    using Children = ExtrusionEntityUPtrs;
    using Content = std::variant<std::monostate, ArcPolyline, Children>;

protected:
    static inline std::atomic_int32_t id_generator;
    static Point NOT_A_POINT;

    uint32_t m_id; // for travel map
    // even if no_sort, allow to reverse() us (and our entities if they allow it, but they should) 
    bool m_can_reverse; //TODO: use (int64_t) m_id sign to embed this property, currently not an issue as 32+8 <= 64
    Content m_content;
    bool    m_can_sort = false;

public:
    explicit ExtrusionEntity(bool can_reverse)
        : m_id(++id_generator), m_can_reverse(can_reverse), m_content(std::monostate{}) {}
    ExtrusionEntity(bool can_reverse, const ArcPolyline &polyline)
        : m_id(++id_generator), m_can_reverse(can_reverse), m_content(polyline) {}
    ExtrusionEntity(bool can_reverse, ArcPolyline &&polyline)
        : m_id(++id_generator), m_can_reverse(can_reverse), m_content(std::move(polyline)) {}
    ExtrusionEntity(Children &&children, bool can_sort, bool can_reverse, bool continuous)
        : m_id(++id_generator), m_can_reverse(can_reverse), m_content(std::move(children)), m_can_sort(can_sort) { (void) continuous; }
    ExtrusionEntity(ExtrusionPropertyUPtr &&eprop, bool can_reverse)
        : ExtrusionPropertyContainer(std::move(eprop)), m_id(++id_generator), m_can_reverse(can_reverse) {}
    ExtrusionEntity(ExtrusionPropertyUPtrs &&eprops, bool can_reverse)
        : ExtrusionPropertyContainer(std::move(eprops)), m_id(++id_generator), m_can_reverse(can_reverse) {}
    ExtrusionEntity(const ExtrusionEntity &rhs)
        : ExtrusionPropertyContainer(rhs)
        , m_id(rhs.m_id)
        , m_can_reverse(rhs.m_can_reverse)
        , m_content(clone_content(rhs.m_content))
        , m_can_sort(rhs.m_can_sort) {}
    ExtrusionEntity(ExtrusionEntity &&rhs)
        : ExtrusionPropertyContainer(std::move(rhs))
        , m_id(rhs.m_id)
        , m_can_reverse(rhs.m_can_reverse)
        , m_content(std::move(rhs.m_content))
        , m_can_sort(rhs.m_can_sort) {}
    
    ExtrusionEntity &operator=(const ExtrusionEntity &rhs) {
        this->m_id = rhs.m_id;
        this->m_can_reverse = rhs.m_can_reverse;
        this->m_content = clone_content(rhs.m_content);
        this->m_can_sort = rhs.m_can_sort;
        ExtrusionPropertyContainer::operator=(rhs);
        return *this;
    }
    ExtrusionEntity &operator=(ExtrusionEntity &&rhs) {
        this->m_id = rhs.m_id;
        this->m_can_reverse = rhs.m_can_reverse;
        this->m_content = std::move(rhs.m_content);
        this->m_can_sort = rhs.m_can_sort;
        ExtrusionPropertyContainer::operator=(std::move(rhs));
        return *this;
    }

protected:
    static Content clone_content(const Content &content);
    Children& ensure_children();

public:
    uint64_t get_id() const { return m_id; }
    bool has_polyline() const;
    bool is_leaf() const;
    bool is_nop() const;
    bool is_continuous() const;

    const ArcPolyline* polyline_or_null() const;
    ArcPolyline* polyline_or_null();
    const ArcPolyline& polyline_ref() const;
    ArcPolyline& polyline_ref();
    void set_polyline(const ArcPolyline &polyline);
    void set_polyline(ArcPolyline &&polyline);

    const Children& children() const;
    Children& children();
    size_t child_count() const;
    const ExtrusionEntity& child(size_t idx) const;
    ExtrusionEntity& child(size_t idx);
    ExtrusionEntity& append_child(ExtrusionEntityUPtr &&child);
    ExtrusionEntity& append_child(const ExtrusionEntity &child);
    ExtrusionEntity& append_child(ExtrusionEntity &&child);
    void insert_child(size_t idx, ExtrusionEntityUPtr &&child);
    void remove_child(size_t idx);
    void clear_content();

    virtual ExtrusionRole role() const;
    virtual bool has_role(ExtrusionRole test_role) const;
    virtual bool is_collection() const;
    virtual bool is_loop() const;
    bool can_sort() const { return m_can_sort && this->is_collection(); }
    virtual bool can_reverse() const { return this->can_sort() || m_can_reverse; }
    void set_can_sort_reverse(bool can_sort, bool can_reverse);
    virtual ExtrusionEntity* clone() const { return new ExtrusionEntity(*this); }
    // Create a new object, initialize it with this object using the move semantics.
    virtual ExtrusionEntity* clone_move() { return new ExtrusionEntity(std::move(*this)); }
    virtual ~ExtrusionEntity() = default;
    virtual void reverse();
    virtual const Point& first_point() const;
    virtual const Point& last_point() const;
    // Returns an approximately middle point of a path, loop or an extrusion collection.
    // Used to get a sample point of an extrusion or extrusion collection, which is possibly deep inside its island.
    virtual const Point& middle_point() const;
    // Produce a list of 2D polygons covered by the extruded paths, offsetted by the extrusion width.
    // Increase the offset by scaled_epsilon to achieve an overlap, so a union will produce no gaps.
    virtual void polygons_covered_by_width(Polygons &out, const float scaled_epsilon) const;
    // Produce a list of 2D polygons covered by the extruded paths, offsetted by the extrusion spacing.
    // Increase the offset by scaled_epsilon to achieve an overlap, so a union will produce no gaps.
    // Useful to calculate area of an infill, which has been really filled in by a 100% rectilinear infill.
    virtual void polygons_covered_by_spacing(Polygons &out, const float spacing_ratio, const float scaled_epsilon) const;
    virtual Polygons polygons_covered_by_width(const float scaled_epsilon = 0.f) const
        { Polygons out; this->polygons_covered_by_width(out, scaled_epsilon); return out; }
    virtual Polygons polygons_covered_by_spacing(const float spacing_ratio, const float scaled_epsilon) const
        { Polygons out; this->polygons_covered_by_spacing(out, spacing_ratio, scaled_epsilon); return out; }
    virtual ArcPolyline as_polyline() const;
    virtual void   collect_polylines(ArcPolylines &dst) const;
    virtual void   collect_points(Points &dst) const;
    virtual ArcPolylines as_polylines() const { ArcPolylines dst; this->collect_polylines(dst); return dst; }
    virtual coordf_t length() const;
    virtual bool empty() const;
    virtual double total_volume() const;
    virtual void visit(ExtrusionVisitor &visitor);
    virtual void visit(ExtrusionVisitorConst &visitor) const;
    void visit(ExtrusionVisitor &&visitor); // note: need 'using ExtrusionEntity::visit;' to be called from children classes
    void visit(ExtrusionVisitorConst &&visitor) const;

};

// only cary an ExtrusionProperty
class ExtrusionNop : public ExtrusionEntity
{
    ExtrusionRole m_role = ExtrusionRole::None;
public:
    static Point NOT_A_POINT;
    // this can have a position, to move the head.
    Point position = NOT_A_POINT;
    ExtrusionNop() : ExtrusionEntity(true) {}
    ExtrusionNop(const ExtrusionNop& other) : ExtrusionEntity(other), m_role(other.m_role), position(other.position) {}
    ExtrusionNop(ExtrusionNop&& other) : ExtrusionEntity(std::move(other)), m_role(other.m_role), position(std::move(other.position)) {}
    template<typename PropertyType>
    explicit ExtrusionNop(const PropertyType &attr) : ExtrusionEntity(true) { this->add_property(attr); }
    ExtrusionNop &operator=(const ExtrusionNop &rhs) {
        ExtrusionEntity::operator=(rhs);
        this->m_role = rhs.m_role;
        this->position = rhs.position;
        return *this;
    }
    ExtrusionNop &operator=(ExtrusionNop &rhs) {
        ExtrusionEntity::operator=(rhs);
        this->m_role = rhs.m_role;
        this->position = rhs.position;
        return *this;
    }
    ExtrusionRole role() const override { return m_role; }
    void set_role(ExtrusionRole new_role) { m_role = new_role; }
    bool has_role(ExtrusionRole test_role) const override { return (m_role & test_role) == test_role; }
    ExtrusionEntity *clone() const override { return new ExtrusionNop(*this); }
    // Create a new object, initialize it with this object using the move semantics.
    virtual ExtrusionEntity* clone_move() { return new ExtrusionNop(std::move(*this)); }
    void reverse() override {}
    const Point &first_point() const override { return position; }
    const Point& last_point() const override { return position; }
    const Point& middle_point() const override { return position; }
    void polygons_covered_by_width(Polygons &out, const float scaled_epsilon) const override {}
    void polygons_covered_by_spacing(Polygons &out, const float spacing_ratio, const float scaled_epsilon) const override {}
    Polygons polygons_covered_by_width(const float scaled_epsilon = 0.f) const override { return {}; }
    Polygons polygons_covered_by_spacing(const float spacing_ratio, const float scaled_epsilon) const override { return {}; }
    ArcPolyline as_polyline() const override { return {}; }
    void collect_polylines(ArcPolylines &dst) const override {}
    void collect_points(Points &dst) const override {}
    coordf_t length() const override { return 0; }
    bool empty() const override { return true; }
    double total_volume() const override { return 0; }
    void visit(ExtrusionVisitor &visitor) override;
    void visit(ExtrusionVisitorConst &visitor) const override;
};

//FIXME: this is unsafe. it's a collection of row pointer that isn't ours
using ExtrusionEntitiesPtr = std::vector<ExtrusionEntity*>;

//FIXME: this is unsafe. it contains a raw pointer that isn't ours
// Const reference for ordering extrusion entities without having to modify them.
class ExtrusionEntityReference final
{
public:
    ExtrusionEntityReference() = delete;
    ExtrusionEntityReference(const ExtrusionEntity &extrusion_entity, bool flipped) : 
        m_extrusion_entity(&extrusion_entity), m_flipped(flipped) {}
    ExtrusionEntityReference operator=(const ExtrusionEntityReference &rhs) 
        { m_extrusion_entity = rhs.m_extrusion_entity; m_flipped = rhs.m_flipped; return *this; }

    const ExtrusionEntity& extrusion_entity() const { return *m_extrusion_entity; }
    template<typename Type>
    const Type*            cast()             const { return dynamic_cast<const Type*>(m_extrusion_entity); }
    bool                   flipped()          const { return m_flipped; }

private:
    const ExtrusionEntity *m_extrusion_entity;
    bool                   m_flipped;
};

//FIXME: this is still unsafe. it's a collection of unsafe container.
using ExtrusionEntityReferences = std::vector<ExtrusionEntityReference>;

class ExtrusionPath : public ExtrusionEntity
{
public:
    // force to set the ExtrusionProperty (to nullptr) to be sure you didn't forget it
    //ExtrusionPath(ExtrusionRole role) : ExtrusionEntity(true), m_attributes{role} {}
    //ExtrusionPath(const ExtrusionAttributes &attributes, bool can_reverse = true) : ExtrusionEntity(can_reverse), m_attributes(attributes) {}
    ExtrusionPath(const ExtrusionAttributes &attributes,
                  ExtrusionPropertyUPtr &&eprop,
                  bool can_reverse = true)
        : ExtrusionEntity(can_reverse, ArcPolyline()) { this->set_attributes(attributes); if (eprop) this->add_property(std::move(eprop)); }
    ExtrusionPath(const ExtrusionAttributes &attributes,
                  ExtrusionPropertyUPtrs &&eprops,
                  bool can_reverse = true)
        : ExtrusionEntity(can_reverse, ArcPolyline()) { this->set_attributes(attributes); for (ExtrusionPropertyUPtr &property : eprops) this->add_property(std::move(property)); }
    ExtrusionPath(const ExtrusionAttributes &attributes,
                  bool can_reverse = true)
        : ExtrusionEntity(can_reverse, ArcPolyline()) { this->set_attributes(attributes); }
    ExtrusionPath(const ExtrusionPath &rhs) : ExtrusionEntity(rhs) {}
    ExtrusionPath(ExtrusionPath &&rhs) : ExtrusionEntity(std::move(rhs)) {}
    //ExtrusionPath(const ArcPolyline &polyline, const ExtrusionAttributes &attribs, bool can_reverse = true)
        //: ExtrusionEntity(can_reverse, polyline), m_attributes(attribs) {}
    //ExtrusionPath(ArcPolyline &&polyline, const ExtrusionAttributes &attribs, bool can_reverse = true)
        //: ExtrusionEntity(can_reverse, std::move(polyline)), m_attributes(attribs) {}
    ExtrusionPath(const ArcPolyline &polyline,
                  const ExtrusionAttributes &attribs,
                  ExtrusionPropertyUPtr &&eprop,
                  bool can_reverse = true)
        : ExtrusionEntity(can_reverse, polyline) { this->set_attributes(attribs); if (eprop) this->add_property(std::move(eprop)); }
    ExtrusionPath(const ArcPolyline &polyline,
                  const ExtrusionAttributes &attribs,
                  ExtrusionPropertyUPtrs &&eprops,
                  bool can_reverse = true)
        : ExtrusionEntity(can_reverse, polyline) { this->set_attributes(attribs); for (ExtrusionPropertyUPtr &property : eprops) this->add_property(std::move(property)); }
    ExtrusionPath(ArcPolyline &&polyline,
                  const ExtrusionAttributes &attribs,
                  ExtrusionPropertyUPtr &&eprop,
                  bool can_reverse = true)
        : ExtrusionEntity(can_reverse, std::move(polyline)) { this->set_attributes(attribs); if (eprop) this->add_property(std::move(eprop)); }
    ExtrusionPath(ArcPolyline &&polyline,
                  const ExtrusionAttributes &attribs,
                  ExtrusionPropertyUPtrs &&eprops,
                  bool can_reverse = true)
        : ExtrusionEntity(can_reverse, std::move(polyline)) { this->set_attributes(attribs); for (ExtrusionPropertyUPtr &property : eprops) this->add_property(std::move(property)); }

    ExtrusionPath &operator=(const ExtrusionPath &rhs) {
        ExtrusionEntity::operator=(rhs);
        return *this;
    }
    ExtrusionPath &operator=(ExtrusionPath &&rhs) {
        ExtrusionEntity::operator=(std::move(rhs));
        return *this;
    }

    ArcPolyline& polyline() { return this->polyline_ref(); }
    const ArcPolyline& polyline() const { return this->polyline_ref(); }

	ExtrusionEntity* clone() const override { return new ExtrusionPath(*this); }
    // Create a new object, initialize it with this object using the move semantics.
    virtual ExtrusionPath* clone_move() override { return new ExtrusionPath(std::move(*this)); }
    void reverse() override { this->polyline().reverse(); }
    void set_can_reverse(bool can_reverse) { this->m_can_reverse = can_reverse; }
    const Point& first_point() const override { return this->polyline().front(); }
    const Point& last_point() const override { return this->polyline().back(); }
    // Is it really what you can call a middle point?: yes, it's more random than middle.
    const Point &middle_point() const override { return this->polyline().middle(); }
    size_t size() const { return this->polyline().size(); }
    bool empty() const { return this->polyline().empty(); }
    bool is_closed() const { return ! this->empty() && this->polyline().front() == this->polyline().back(); }
    // Produce a list of extrusion paths into retval by clipping this path by ExPolygons.
    // Currently not used.
    void intersect_expolygons(const ExPolygons &collection, ExtrusionEntityCollection* retval) const;
    // Produce a list of extrusion paths into retval by removing parts of this path by ExPolygons.
    // Currently not used.
    void subtract_expolygons(const ExPolygons &collection, ExtrusionEntityCollection* retval) const;
    void clip_end(coordf_t distance);
    coordf_t length() const override;

    const ExtrusionAttributes&  attributes() const { const ExtrusionAttributes *attributes = this->get_property<ExtrusionAttributes>(); assert(attributes != nullptr); return *attributes; }
    ExtrusionRole               role() const override { return attributes().extrusion_role(); }
    bool has_role(ExtrusionRole test_role) const override { return (attributes().extrusion_role() & test_role) == test_role; }
    float                       width() const { return attributes().width; }
    float                       height() const { return attributes().height; }
    double                      mm3_per_mm() const { return attributes().mm3_per_mm; }
    // Minimum volumetric velocity of this extrusion entity. Used by the constant nozzle pressure algorithm.
    double                      min_mm3_per_mm() const { return attributes().mm3_per_mm; }
    ExtrusionPropertyOverhang &overhang_attributes_mutable();
    const ExtrusionPropertyOverhang *overhang_attributes() const; // can be null if not present
    ExtrusionAttributes& attributes_mutable() { return this->get_or_add_property<ExtrusionAttributes>(); }

    void set_role(ExtrusionRole new_role) { attributes_mutable().set_role(new_role); }
    // Produce a list of 2D polygons covered by the extruded paths, offsetted by the extrusion width.
    // Increase the offset by scaled_epsilon to achieve an overlap, so a union will produce no gaps.
    void polygons_covered_by_width(Polygons &out, const float scaled_epsilon) const override;
    // Produce a list of 2D polygons covered by the extruded paths, offsetted by the extrusion spacing.
    // Increase the offset by scaled_epsilon to achieve an overlap, so a union will produce no gaps.
    // Useful to calculate area of an infill, which has been really filled in by a 100% rectilinear infill.
    void polygons_covered_by_spacing(Polygons &out, const float spacing_ratio, const float scaled_epsilon) const override;
    virtual Polygons polygons_covered_by_width(const float scaled_epsilon = 0.f) const
        { Polygons out; this->polygons_covered_by_width(out, scaled_epsilon); return out; }
    virtual Polygons polygons_covered_by_spacing(const float spacing_ratio, const float scaled_epsilon) const
        { Polygons out; this->polygons_covered_by_spacing(out, spacing_ratio, scaled_epsilon); return out; }
    ArcPolyline as_polyline() const override { return this->polyline(); }
    void          collect_polylines(ArcPolylines &dst) const override { if (! this->polyline().empty()) dst.emplace_back(this->polyline()); }
    void          collect_points(Points &dst) const override { append(dst, this->polyline().to_polyline().points); }
    double      total_volume() const override { return attributes().mm3_per_mm * unscaled(length()); }
    void push_back(Point point, coord_t z_offset) {
        assert(!this->polyline().has_arc());
        this->polyline().append(point);
        this->polyline().set_z_offset(this->polyline().size() - 1, z_offset);
    }
    void push_back(const Geometry::ArcWelder::Segment &segment, coord_t z_offset) {
        assert(!this->polyline().has_arc() || segment.orientation == Geometry::ArcWelder::Orientation::Unknown);
        this->polyline().append(segment);
        this->polyline().set_z_offset(this->polyline().size() - 1, z_offset);
    }
    using ExtrusionEntity::visit;
    virtual void visit(ExtrusionVisitor &visitor) override;
    virtual void visit(ExtrusionVisitorConst &visitor) const override;

protected:
    void _inflate_collection(const Polylines &polylines, ExtrusionEntityCollection* collection) const;

    void set_attributes(const ExtrusionAttributes &attributes) { this->add_property(attributes); }
};
/* just set the path to can_reverse = false
class ExtrusionPathOriented : public ExtrusionPath
{
public:
    ExtrusionPathOriented(const ExtrusionAttributes &attribs) : ExtrusionPath(attribs) {}
    ExtrusionPathOriented(const Polyline &polyline, const ExtrusionAttributes &attribs) : ExtrusionPath(polyline, attribs) {}
    ExtrusionPathOriented(Polyline &&polyline, const ExtrusionAttributes &attribs) : ExtrusionPath(std::move(polyline), attribs) {}

    ExtrusionEntity* clone() const override { return new ExtrusionPathOriented(*this); }
    // Create a new object, initialize it with this object using the move semantics.
    ExtrusionEntity* clone_move() override { return new ExtrusionPathOriented(std::move(*this)); }
    virtual bool can_reverse() const override { return false; }
};
*/

typedef std::vector<ExtrusionPath> ExtrusionPaths;
ExtrusionPaths clip_end(ExtrusionPaths& paths, coordf_t distance);

template<bool IsConst>
class ExtrusionPathIterator
{
    using ChildrenPtr = std::conditional_t<IsConst, const ExtrusionEntityUPtrs*, ExtrusionEntityUPtrs*>;
    using EntityPtr = std::conditional_t<IsConst, const ExtrusionEntity*, ExtrusionEntity*>;

    ChildrenPtr m_children = nullptr;
    size_t      m_index = 0;

public:
    using iterator_category = std::random_access_iterator_tag;
    using value_type        = ExtrusionPath;
    using difference_type   = std::ptrdiff_t;
    using reference         = std::conditional_t<IsConst, const ExtrusionPath&, ExtrusionPath&>;
    using pointer           = std::conditional_t<IsConst, const ExtrusionPath*, ExtrusionPath*>;

    ExtrusionPathIterator() = default;
    ExtrusionPathIterator(ChildrenPtr children, size_t index) : m_children(children), m_index(index) {}
    template<bool OtherConst, typename = std::enable_if_t<IsConst && !OtherConst>>
    ExtrusionPathIterator(const ExtrusionPathIterator<OtherConst> &other) : m_children(other.children()), m_index(other.index()) {}

    ChildrenPtr children() const { return m_children; }
    size_t index() const { return m_index; }

    reference operator*() const
    {
        EntityPtr entity = (*m_children)[m_index].get();
        assert(dynamic_cast<pointer>(entity) != nullptr);
        return *static_cast<pointer>(entity);
    }
    pointer operator->() const { return &**this; }
    reference operator[](difference_type offset) const { return *(*this + offset); }

    ExtrusionPathIterator& operator++() { ++m_index; return *this; }
    ExtrusionPathIterator operator++(int) { ExtrusionPathIterator out = *this; ++*this; return out; }
    ExtrusionPathIterator& operator--() { --m_index; return *this; }
    ExtrusionPathIterator operator--(int) { ExtrusionPathIterator out = *this; --*this; return out; }
    ExtrusionPathIterator& operator+=(difference_type offset) { m_index = size_t(difference_type(m_index) + offset); return *this; }
    ExtrusionPathIterator& operator-=(difference_type offset) { return *this += -offset; }
    ExtrusionPathIterator operator+(difference_type offset) const { ExtrusionPathIterator out = *this; out += offset; return out; }
    ExtrusionPathIterator operator-(difference_type offset) const { ExtrusionPathIterator out = *this; out -= offset; return out; }
    difference_type operator-(const ExtrusionPathIterator &rhs) const { assert(m_children == rhs.m_children); return difference_type(m_index) - difference_type(rhs.m_index); }

    bool operator==(const ExtrusionPathIterator &rhs) const { return m_children == rhs.m_children && m_index == rhs.m_index; }
    bool operator!=(const ExtrusionPathIterator &rhs) const { return !(*this == rhs); }
    bool operator<(const ExtrusionPathIterator &rhs) const { assert(m_children == rhs.m_children); return m_index < rhs.m_index; }
    bool operator>(const ExtrusionPathIterator &rhs) const { return rhs < *this; }
    bool operator<=(const ExtrusionPathIterator &rhs) const { return !(*this > rhs); }
    bool operator>=(const ExtrusionPathIterator &rhs) const { return !(*this < rhs); }
};

class ExtrusionPathCView
{
protected:
    const ExtrusionEntityUPtrs *m_children = nullptr;

public:
    using const_iterator = ExtrusionPathIterator<true>;

    explicit ExtrusionPathCView(const ExtrusionEntityUPtrs &children) : m_children(&children) {}

    size_t size() const { return m_children->size(); }
    bool empty() const { return m_children->empty(); }
    const_iterator begin() const { return const_iterator(m_children, 0); }
    const_iterator end() const { return const_iterator(m_children, m_children->size()); }
    const ExtrusionPath& operator[](size_t idx) const { return *(this->begin() + std::ptrdiff_t(idx)); }
    const ExtrusionPath& front() const { return (*this)[0]; }
    const ExtrusionPath& back() const { return (*this)[this->size() - 1]; }
    ExtrusionPaths to_vector() const
    {
        ExtrusionPaths out;
        out.reserve(this->size());
        for (const ExtrusionPath &path : *this)
            out.emplace_back(path);
        return out;
    }
    operator ExtrusionPaths() const { return this->to_vector(); }
};

class ExtrusionPathView : public ExtrusionPathCView
{
    ExtrusionEntityUPtrs *mutable_children() { return const_cast<ExtrusionEntityUPtrs*>(m_children); }

public:
    using iterator = ExtrusionPathIterator<false>;
    using const_iterator = ExtrusionPathIterator<true>;

    explicit ExtrusionPathView(ExtrusionEntityUPtrs &children) : ExtrusionPathCView(children) {}

    iterator begin() { return iterator(this->mutable_children(), 0); }
    iterator end() { return iterator(this->mutable_children(), this->size()); }
    const_iterator begin() const { return ExtrusionPathCView::begin(); }
    const_iterator end() const { return ExtrusionPathCView::end(); }
    ExtrusionPath& operator[](size_t idx) { return *(this->begin() + std::ptrdiff_t(idx)); }
    const ExtrusionPath& operator[](size_t idx) const { return ExtrusionPathCView::operator[](idx); }
    ExtrusionPath& front() { return (*this)[0]; }
    const ExtrusionPath& front() const { return ExtrusionPathCView::front(); }
    ExtrusionPath& back() { return (*this)[this->size() - 1]; }
    const ExtrusionPath& back() const { return ExtrusionPathCView::back(); }

    void reserve(size_t size) { this->mutable_children()->reserve(size); }
    void clear() { this->mutable_children()->clear(); }

    template<typename... Args> ExtrusionPath& emplace_back(Args&&... args)
    {
        ExtrusionEntityUPtrs &children = *this->mutable_children();
        children.emplace_back(std::make_unique<ExtrusionPath>(std::forward<Args>(args)...));
        return *static_cast<ExtrusionPath*>(children.back().get());
    }
    void push_back(const ExtrusionPath &path) { this->emplace_back(path); }
    void push_back(ExtrusionPath &&path) { this->emplace_back(std::move(path)); }
    void pop_back() { this->mutable_children()->pop_back(); }

    iterator insert(iterator pos, const ExtrusionPath &path)
    {
        ExtrusionEntityUPtrs &children = *this->mutable_children();
        size_t idx = pos.index();
        children.insert(children.begin() + idx, std::make_unique<ExtrusionPath>(path));
        return iterator(&children, idx);
    }
    iterator insert(iterator pos, ExtrusionPath &&path)
    {
        ExtrusionEntityUPtrs &children = *this->mutable_children();
        size_t idx = pos.index();
        children.insert(children.begin() + idx, std::make_unique<ExtrusionPath>(std::move(path)));
        return iterator(&children, idx);
    }
    template<typename InputIt> iterator insert(iterator pos, InputIt first, InputIt last)
    {
        ExtrusionEntityUPtrs &children = *this->mutable_children();
        size_t insert_idx = pos.index();
        size_t first_idx = insert_idx;
        for (InputIt it = first; it != last; ++it, ++insert_idx)
            children.insert(children.begin() + insert_idx, std::make_unique<ExtrusionPath>(*it));
        return iterator(&children, first_idx);
    }
    iterator erase(iterator pos)
    {
        ExtrusionEntityUPtrs &children = *this->mutable_children();
        size_t idx = pos.index();
        children.erase(children.begin() + idx);
        return iterator(&children, idx);
    }
    iterator erase(iterator first, iterator last)
    {
        ExtrusionEntityUPtrs &children = *this->mutable_children();
        size_t idx = first.index();
        children.erase(children.begin() + idx, children.begin() + last.index());
        return iterator(&children, idx);
    }
    ExtrusionPathView& operator=(const ExtrusionPaths &paths)
    {
        this->clear();
        this->reserve(paths.size());
        for (const ExtrusionPath &path : paths)
            this->push_back(path);
        return *this;
    }
    ExtrusionPathView& operator=(ExtrusionPaths &&paths)
    {
        this->clear();
        this->reserve(paths.size());
        for (ExtrusionPath &path : paths)
            this->push_back(std::move(path));
        paths.clear();
        return *this;
    }
};

// Single continuous extrusion path, possibly with varying extrusion thickness, extrusion height or bridging / non bridging.
// it's like an unsortable collection of only unreversable THING
// note: the ExtrusionProperty of a multipath is applied to each path, properties of a path is not transfered to the next one.
template <typename THING = ExtrusionEntity>
class ExtrusionMultiEntity : public ExtrusionEntity {
public:
    ExtrusionMultiEntity(): ExtrusionEntity(ExtrusionEntity::Children(), false, false, true) {};
    ExtrusionMultiEntity(const ExtrusionMultiEntity &rhs) : ExtrusionEntity(rhs) {}
    ExtrusionMultiEntity(ExtrusionMultiEntity &&rhs) : ExtrusionEntity(std::move(rhs)) {}
    ExtrusionMultiEntity(const std::vector<THING> &paths) : ExtrusionEntity(ExtrusionEntity::Children(), false, false, true) { this->paths() = paths; };
    ExtrusionMultiEntity(const THING &path): ExtrusionEntity(ExtrusionEntity::Children(), false, false, true) { this->paths().push_back(path); }

    ExtrusionMultiEntity &operator=(const ExtrusionMultiEntity &rhs) {
        ExtrusionEntity::operator=(rhs);
        return *this;
    }
    ExtrusionMultiEntity &operator=(ExtrusionMultiEntity &&rhs) {
        ExtrusionEntity::operator=(std::move(rhs));
        return *this;
    }

    ExtrusionPathView paths() { return ExtrusionPathView(this->children()); }
    ExtrusionPathCView paths() const { assert(!this->is_leaf()); return ExtrusionPathCView(this->children()); }

    bool is_loop() const override { return false; }
    virtual const Point& first_point() const override { return this->paths().front().polyline().front(); }
    virtual const Point& last_point() const override { return this->paths().back().polyline().back(); }

    virtual void reverse() override {
        for (THING &entity : this->paths())
            entity.reverse();
        std::reverse(this->paths().begin(), this->paths().end());
    }
    ExtrusionRole role() const override
    {
        if (this->paths().empty())
            return ExtrusionRole::None;
        ExtrusionRole role = this->paths().front().role();
        for (const ExtrusionPath &path : this->paths())
            if (role != path.role()) {
                return ExtrusionRole::Mixed;
            }
        return role;
    }
    bool has_role(ExtrusionRole test_role) const override {
        if (this->paths().empty())
            return false;
        for (const ExtrusionPath &path : this->paths())
            if (path.has_role(test_role)) {
                return true;
            }
        return false;
    }


    // Is it really what you can call a middle point?:
    const Point& middle_point() const override { const THING &path = this->paths()[this->paths().size() / 2]; return path.polyline().middle(); }
    size_t size() const { return this->paths().size(); }
    coordf_t length() const override {
        coordf_t len = 0;
        for (const THING &entity : this->paths())
            len += entity.length();
        return len;
    }
    bool empty() const override {
        for (const THING &entity : this->paths())
            if (!entity.empty())
                return false;
        return true;
    }

    // Produce a list of 2D polygons covered by the extruded paths, offsetted by the extrusion width.
    // Increase the offset by scaled_epsilon to achieve an overlap, so a union will produce no gaps.
    void polygons_covered_by_width(Polygons &out, const float scaled_epsilon) const override {
        for (const THING &entity : this->paths())
            entity.polygons_covered_by_width(out, scaled_epsilon);
    }

    // Produce a list of 2D polygons covered by the extruded paths, offsetted by the extrusion spacing.
    // Increase the offset by scaled_epsilon to achieve an overlap, so a union will produce no gaps.
    // Useful to calculate area of an infill, which has been really filled in by a 100% rectilinear infill.
    void polygons_covered_by_spacing(Polygons &out, const float spacing_ratio, const float scaled_epsilon) const override {
        for (const THING &entity : this->paths())
            entity.polygons_covered_by_spacing(out, spacing_ratio, scaled_epsilon);
    }

    ArcPolyline as_polyline() const override {
        ArcPolyline out;
        if (!paths().empty()) {
            out = paths().front().as_polyline();
            for (size_t i = 1; i < paths().size(); ++i) {
                out.append(paths()[i].as_polyline());
            }
        }
        return out;
    }
    Polygons polygons_covered_by_width(const float scaled_epsilon = 0.f) const override{ Polygons out; this->polygons_covered_by_width(out, scaled_epsilon); return out; }
    Polygons polygons_covered_by_spacing(const float spacing_ratio, const float scaled_epsilon) const override { Polygons out; this->polygons_covered_by_spacing(out, spacing_ratio,  scaled_epsilon); return out; }
    void collect_polylines(ArcPolylines &dst) const override { ArcPolyline pl = this->as_polyline(); if (!pl.empty()) dst.emplace_back(std::move(pl)); }
    void collect_points(Points &dst) const override { 
        size_t n = std::accumulate(paths().begin(), paths().end(), 0, [](const size_t n, const ExtrusionPath &p){ return n + p.polyline().size(); });
        dst.reserve(dst.size() + n);
        for (const ExtrusionPath &p : this->paths())
            append(dst, p.polyline().to_polyline().points);
    }
    double total_volume() const override { double volume = 0.; for (const auto& path : paths()) volume += path.total_volume(); return volume; }
};

// Single continuous extrusion path, possibly with varying extrusion thickness, extrusion height or bridging / non bridging.
// it's like an unsortable collection of only unreversable ExtrusionPaths
class ExtrusionMultiPath : public ExtrusionMultiEntity<ExtrusionPath> {
public:

    ExtrusionMultiPath() {};
    ExtrusionMultiPath(const ExtrusionMultiPath &rhs) : ExtrusionMultiEntity(rhs) {}
    ExtrusionMultiPath(ExtrusionMultiPath &&rhs) : ExtrusionMultiEntity(std::move(rhs)) {}
    ExtrusionMultiPath(const ExtrusionPaths &paths) : ExtrusionMultiEntity(paths) {};
    ExtrusionMultiPath(const ExtrusionPath &path) :ExtrusionMultiEntity(path) {}

    ExtrusionMultiPath &operator=(const ExtrusionMultiPath &rhs) {
        ExtrusionEntity::operator=(rhs);
        return *this;
    }
    ExtrusionMultiPath &operator=(ExtrusionMultiPath &&rhs) {
        ExtrusionEntity::operator=(std::move(rhs));
        return *this;
    }

    void set_can_reverse(bool can_reverse) { m_can_reverse = can_reverse; }

    virtual ExtrusionMultiPath* clone() const override { return new ExtrusionMultiPath(*this); }
    virtual ExtrusionMultiPath* clone_move() override { return new ExtrusionMultiPath(std::move(*this)); }

    using ExtrusionEntity::visit;
    virtual void visit(ExtrusionVisitor &visitor) override;
    virtual void visit(ExtrusionVisitorConst &visitor) const override;
};
// Single continuous extrusion loop, possibly with varying extrusion thickness, extrusion height or bridging / non bridging.
// note: the ExtrusionProperty of a multipath is applied to each path, properties of a path is not transfered to the next one.
class ExtrusionLoop : public ExtrusionEntity
{
public:
    //ExtrusionLoop(const ExtrusionLoop& rhs) : ExtrusionEntity(rhs), m_loop_role(rhs.m_loop_role) {}
    ExtrusionLoop(ExtrusionLoopRole role = elrDefault) : ExtrusionEntity(ExtrusionEntity::Children(), false, false, true) { this->set_loop_role(role); }
    ExtrusionLoop(const ExtrusionPaths &paths, ExtrusionLoopRole role = elrDefault) : ExtrusionEntity(ExtrusionEntity::Children(), false, false, true) {
        this->set_loop_role(role);
        this->paths() = paths;
        assert(!this->paths().empty());
        assert(this->first_point().coincides_with_epsilon(this->paths().back().polyline().back()));
    }
    ExtrusionLoop(ExtrusionPaths &&paths, ExtrusionLoopRole role = elrDefault) : ExtrusionEntity(ExtrusionEntity::Children(), false, false, true) {
        this->set_loop_role(role);
        this->paths() = std::move(paths);
        assert(!this->paths().empty());
        assert(this->first_point().coincides_with_epsilon(this->paths().back().polyline().back()));
    }
    ExtrusionLoop(const ExtrusionPath &path, ExtrusionLoopRole role = elrDefault) : ExtrusionEntity(ExtrusionEntity::Children(), false, false, true) {
        this->set_loop_role(role);
        this->paths().push_back(path);
        assert(!this->paths().empty());
        assert(this->first_point().coincides_with_epsilon(this->paths().back().polyline().back()));
    }
    ExtrusionLoop(ExtrusionPath &&path, ExtrusionLoopRole role = elrDefault) : ExtrusionEntity(ExtrusionEntity::Children(), false, false, true) {
        this->set_loop_role(role);
        this->paths().emplace_back(std::move(path));
        assert(!this->paths().empty());
        assert(this->first_point().coincides_with_epsilon(this->paths().back().polyline().back()));
    }
    ExtrusionPathView paths() { return ExtrusionPathView(this->children()); }
    ExtrusionPathCView paths() const { assert(!this->is_leaf()); return ExtrusionPathCView(this->children()); }
    virtual bool is_loop() const override{ return true; }
    virtual ExtrusionEntity* clone() const override{ return new ExtrusionLoop (*this); }
    // Create a new object, initialize it with this object using the move semantics.
    virtual ExtrusionEntity* clone_move() override { return new ExtrusionLoop(std::move(*this)); }
    double          area() const;
    bool            is_counter_clockwise() const;
    bool            is_clockwise() const;
    // Used by PerimeterGenerator to reorient extrusion loops. (old make_clockwise() and make_counter_clockwise())
    void            reverse() override;
    const Point&    first_point() const override { return this->paths().front().polyline().front(); }
    const Point&    last_point() const override { assert(this->first_point() == this->paths().back().polyline().back()); return this->first_point(); }
    // Is it really what you can call a middle point?: 
    const Point&    middle_point() const override { const ExtrusionPath &path = this->paths()[this->paths().size() / 2]; return path.polyline().middle(); }
    Polygon polygon() const;
    coordf_t length() const override;
    bool empty() const override {
        for (const ExtrusionPath &path : paths())
            if (!path.empty())
                return false;
        return true;
    }
    bool split_at_vertex(const Point &point, const coordf_t scaled_epsilon = scale_d(0.001));
    void split_at(const Point &point, bool prefer_non_overhang, const coordf_t scaled_epsilon = scale_d(0.001));
    struct ClosestPathPoint {
        size_t path_idx;
        size_t segment_idx;
        Point  foot_pt;
    };
    ClosestPathPoint get_closest_path_and_point(const Point& point, bool prefer_non_overhang) const;
    // Test, whether the point is extruded by a bridging flow.
    // This used to be used to avoid placing seams on overhangs, but now the EdgeGrid is used instead.
    //bool has_overhang_point(const Point &point) const;
    ExtrusionRole role() const override;
    bool has_role(ExtrusionRole test_role) const override;
    ExtrusionLoopRole loop_role() const
    {
        const ExtrusionPropertyLoopRole *property = this->get_property<ExtrusionPropertyLoopRole>();
        return property == nullptr ? elrDefault : property->perimeter_role();
    }
    // Produce a list of 2D polygons covered by the extruded paths, offsetted by the extrusion width.
    // Increase the offset by scaled_epsilon to achieve an overlap, so a union will produce no gaps.
    void polygons_covered_by_width(Polygons &out, const float scaled_epsilon) const override;
    // Produce a list of 2D polygons covered by the extruded paths, offsetted by the extrusion spacing.
    // Increase the offset by scaled_epsilon to achieve an overlap, so a union will produce no gaps.
    // Useful to calculate area of an infill, which has been really filled in by a 100% rectilinear infill.
    void polygons_covered_by_spacing(Polygons &out, const float spacing_ratio, const float scaled_epsilon) const  override;
    Polygons polygons_covered_by_width(const float scaled_epsilon = 0.f) const
        { Polygons out; this->polygons_covered_by_width(out, scaled_epsilon); return out; }
    Polygons polygons_covered_by_spacing(const float spacing_ratio, const float scaled_epsilon) const
        { Polygons out; this->polygons_covered_by_spacing(out, spacing_ratio, scaled_epsilon); return out; }
    ArcPolyline as_polyline() const override;
    void   collect_polylines(ArcPolylines &dst) const override { ArcPolyline pl = this->as_polyline(); if (! pl.empty()) dst.emplace_back(std::move(pl)); }
    void   collect_points(Points &dst) const override { 
        size_t n = std::accumulate(paths().begin(), paths().end(), 0, [](const size_t n, const ExtrusionPath &p){ return n + p.polyline().size(); });
        dst.reserve(dst.size() + n);
        for (const ExtrusionPath &p : this->paths())
            append(dst, p.as_polyline().to_polyline().points);
    }
    double total_volume() const override { double volume =0.; for (const auto& path : paths()) volume += path.total_volume(); return volume; }

    using ExtrusionEntity::visit;
    virtual void visit(ExtrusionVisitor &visitor) override;
    virtual void visit(ExtrusionVisitorConst &visitor) const override;

#ifndef NDEBUG
	bool validate() const {
		assert(this->first_point() == this->paths().back().polyline().back());
		for (size_t i = 1; i < paths().size(); ++ i)
			assert(this->paths()[i - 1].polyline().back() == this->paths()[i].polyline().front());
		return true;
	}
#endif /* NDEBUG */

private:
    void set_loop_role(ExtrusionLoopRole role)
    {
        if (role == elrDefault)
            this->remove_property<ExtrusionPropertyLoopRole>();
        else
            this->get_or_add_property<ExtrusionPropertyLoopRole>().set_perimeter_role(role);
    }
};

Polygon polygon(const ExtrusionEntity &entity);

inline void extrusion_paths_append(ExtrusionPaths &dst, Polylines &polylines, const ExtrusionAttributes &attributes, bool can_reverse = true)
{
    dst.reserve(dst.size() + polylines.size());
    for (Polyline &polyline : polylines) {
        assert(polyline.is_valid());
        if (polyline.is_valid())
            dst.emplace_back(polyline, attributes, nullptr, can_reverse);
    }
}

inline void extrusion_paths_append(ExtrusionPaths &dst, Polylines &&polylines, const ExtrusionAttributes &attributes, bool can_reverse = true)
{
    dst.reserve(dst.size() + polylines.size());
    for (Polyline &polyline : polylines) {
        assert(polyline.is_valid());
        if (polyline.is_valid())
            dst.emplace_back(std::move(polyline), attributes, nullptr, can_reverse);
    }
    polylines.clear();
}
inline void extrusion_paths_append(ExtrusionPaths &dst,
                                   Polylines &polylines,
                                   const ExtrusionAttributes &attributes,
                                   const ExtrusionPropertyOverhang &overhangs_attr,
                                   bool can_reverse = true) {
    dst.reserve(dst.size() + polylines.size());
    for (Polyline &polyline : polylines) {
        assert(polyline.is_valid());
        if (polyline.is_valid())
            dst.emplace_back(polyline, attributes, overhangs_attr.clone(), can_reverse);
    }
}

inline void extrusion_paths_append(ExtrusionPaths &dst,
                                   Polylines &&polylines,
                                   const ExtrusionAttributes &attributes,
                                   const ExtrusionPropertyOverhang &overhangs_attr,
                                   bool can_reverse = true) {
    dst.reserve(dst.size() + polylines.size());
    for (Polyline &polyline : polylines) {
        assert(polyline.is_valid());
        if (polyline.is_valid())
            dst.emplace_back(std::move(polyline), attributes, overhangs_attr.clone(), can_reverse);
    }
    polylines.clear();
}

}

#endif
