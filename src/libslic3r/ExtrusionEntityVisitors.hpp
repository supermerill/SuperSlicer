///|/ Copyright (c) SuperSlicer 2026 Durand Rémi @supermerill
///|/ Copyright (c) Prusa Research 2016 - 2023 Vojtěch Bubník @bubnikv, Lukáš Matěna @lukasmatena, Enrico Turri @enricoturri1966
///|/ Copyright (c) Slic3r 2013 - 2016 Alessandro Ranellucci @alranel
///|/
///|/ PrusaSlicer is released under the terms of the AGPLv3 or higher
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/
#ifndef slic3r_ExtrusionEntityVisitors_hpp_
#define slic3r_ExtrusionEntityVisitors_hpp_

#include <cassert>
#include <sstream>
#include <vector>

#include "ExtrusionEntityCollection.hpp"

namespace Slic3r {

/// Visitor helpers for ExtrusionEntity live outside ExtrusionEntity.hpp so most
/// extrusion users don't need to rebuild when a traversal helper is added or
/// changed. Include this header only in code that defines or uses visitors.

class ExtrusionVisitor {
public:
    virtual void default_use(ExtrusionEntity &entity) { assert(false); };
    virtual void use(ExtrusionPath &path);
    virtual void use(ExtrusionMultiPath &multipath);
    virtual void use(ExtrusionLoop &loop);
    virtual void use(ExtrusionEntityCollection &collection);
    virtual void use(ExtrusionNop &nop);
};
class ExtrusionVisitorConst {
public:
    virtual void default_use(const ExtrusionEntity &entity) { assert(false); };
    virtual void use(const ExtrusionPath &path);
    virtual void use(const ExtrusionMultiPath &multipath);
    virtual void use(const ExtrusionLoop &loop);
    virtual void use(const ExtrusionEntityCollection &collection);
    virtual void use(const ExtrusionNop &nop);
};

/// Depth-first helper for walking an ExtrusionEntity tree while keeping the
/// inherited property context easy to query.
///
/// Traversal order is:
///   enter_node(entity)
///   children, from index 0 to child_count() - 1
///   visit_leaf(entity) if the entity has no child
///   leave_node(entity)
///
/// LeafIsNode controls whether leaves also receive enter_node()/leave_node():
///   - LeafIsNode == true:
///       leaves are treated like regular nodes:
///       enter_node(leaf), visit_leaf(leaf), leave_node(leaf)
///   - LeafIsNode == false:
///       leaves receive only visit_leaf(leaf)
///
/// current_property<PropertyType>() returns the nearest active property of this
/// type, starting from the current entity and walking back to the root. This is
/// useful for inherited extrusion state such as speed, flow, modifiers, etc.
/// The returned pointer is const on purpose: inherited parent properties should
/// not be mutated accidentally by a child visitor.
///
/// Structural mutation rules:
///   - Children added in enter_node() are visited, because child iteration starts
///     after enter_node() returns.
///   - Children added/removed/reordered in visit_leaf() or leave_node() are not
///     visited by the current traversal.
///   - If an algorithm needs heavy tree restructuring, prefer a dedicated pass
///     with explicit index management.
template<class Entity, bool LeafIsNode = true>
class ExtrusionTreeVisitorBase {
public:
    /// Traverse root and all its descendants in depth-first order.
    /// The visitor object keeps traversal state, so do not call traverse()
    /// recursively from one of the callbacks.
    void traverse(Entity &root) {
        m_stack.clear();

        m_stack.push_back({ &root });
        while (!m_stack.empty()) {
            Frame &frame = m_stack.back();
            Entity &entity = *frame.entity;

            if (!frame.entered) {
                if constexpr (LeafIsNode) {
                    this->enter_node(entity);
                } else if (entity.child_count() > 0) {
                    this->enter_node(entity);
                }
                frame.entered = true;
            }

            if (frame.next_child < entity.child_count()) {
                m_stack.push_back({ &entity.child(frame.next_child++) });
                continue;
            }

            if (entity.child_count() == 0) {
                this->visit_leaf(entity);
                if constexpr (LeafIsNode) {
                    this->leave_node(entity);
                }
            } else {
                this->leave_node(entity);
            }
            m_stack.pop_back();
        }
    }
protected:
    /// Called before visiting children.
    /// Use this to push/assign state for the current subtree, or to create
    /// children that should be visited by this traversal.
    virtual void enter_node(Entity &entity) {};

    /// Called for entities with no child.
    /// Use this for operations that apply to actual extrusion leaves, typically
    /// entities carrying a polyline or a terminal command/property.
    virtual void visit_leaf(Entity& entity) {};

    /// Called after all children have been visited.
    /// Use this for post-order aggregation, cleanup, or validation.
    virtual void leave_node(Entity& entity) {};

    /// Return the active property of PropertyType for the current traversal
    /// position, or nullptr if no such property is active.
    ///
    /// Lookup starts at the current entity, then climbs toward the root. This
    /// means a child property overrides a parent property of the same type.
    template<class PropertyType>
    const PropertyType* current_property() const
    {
        for (auto it = m_stack.rbegin(); it != m_stack.rend(); ++it) {
            if (const PropertyType *property = it->entity->template get_property<PropertyType>())
                return property;
        }
        return nullptr;
    }

private:
    struct Frame {
        Entity *entity;
        size_t next_child = 0;
        bool entered = false;
    };
    std::vector<Frame> m_stack;
};

/// Mutable extrusion tree visitor.
/// Use this when the pass needs to edit entities, properties, polylines,
/// or children according to the structural mutation rules above.
template<bool LeafIsNode = true>
class ExtrusionTreeVisitor : public ExtrusionTreeVisitorBase<ExtrusionEntity, LeafIsNode>
{};

/// Read-only extrusion tree visitor.
/// Use this for analysis, validation, measurement, logging, or any pass that
/// must not modify the extrusion tree.
template<bool LeafIsNode = true>
class ExtrusionTreeConstVisitor : public ExtrusionTreeVisitorBase<const ExtrusionEntity, LeafIsNode>
{};

class ExtrusionPrinter : public ExtrusionTreeConstVisitor<true> {
    std::stringstream ss;
    std::vector<bool> m_first_child_stack;
    double mult;
    int trunc;
    bool json;
    void begin_entity();
    void print_leaf(const ExtrusionEntity& entity);
    // Print built-in properties stored directly on one entity. Custom plugin
    // properties are skipped because this debug printer does not know their
    // payload layout.
    bool print_properties(const ExtrusionEntity& entity, const char *prefix = "", const char *suffix = "");
    void begin_property(bool &first_property, const char *name);
    void begin_property_field(bool &first_field, const char *name);
    void print_bool_value(bool value);
    void print_string_value(const std::string &value);
    void print_equals();
public:
    ExtrusionPrinter(double mult = 0.000001, int trunc = 0, bool json = false) : mult(mult), trunc(trunc), json(json) { }
    void enter_node(const ExtrusionEntity& entity) override;
    void visit_leaf(const ExtrusionEntity& entity) override;
    void leave_node(const ExtrusionEntity& entity) override;
    std::string str() { return ss.str(); }
    std::string print(const ExtrusionEntity& entity)&& {
        this->traverse(entity);
        return ss.str();
    }
};

class ExtrusionLength : public ExtrusionTreeConstVisitor<false> {
    coordf_t dist;
public:
    ExtrusionLength() : dist(0){ }
    void visit_leaf(const ExtrusionEntity& entity) override;
    double get() { return dist; }
    double length(const ExtrusionEntity& entity)&& {
        this->traverse(entity);
        return get();
    }
};

class ExtrusionVisitorRecursiveConst : public ExtrusionVisitorConst {
public:
    virtual void default_use(const ExtrusionEntity& entity) override;
};

class ExtrusionVisitorRecursive : public ExtrusionVisitor {
public:
    virtual void default_use(ExtrusionEntity& entity) override;
};

class HasRoleVisitor : public ExtrusionTreeConstVisitor<false> {
protected:
    virtual bool matches(const ExtrusionEntity &entity, ExtrusionRole role) const = 0;
public:
    bool found = false;
    void visit_leaf(const ExtrusionEntity& entity) override;
    static bool search(const ExtrusionEntity &entity, HasRoleVisitor&& visitor);
    static bool search(const ExtrusionEntitiesPtr &entities, HasRoleVisitor&& visitor);
};
struct HasInfillVisitor : public HasRoleVisitor{
    bool matches(const ExtrusionEntity&, ExtrusionRole role) const override { return role.is_infill(); }
};
struct HasSolidInfillVisitor : public HasRoleVisitor{
    bool matches(const ExtrusionEntity&, ExtrusionRole role) const override { return role.is_solid_infill(); }
};
struct HasThisRoleVisitor : public HasRoleVisitor{
    ExtrusionRole role_to_find;
    HasThisRoleVisitor(ExtrusionRole role) : role_to_find(role) {}
    bool matches(const ExtrusionEntity&, ExtrusionRole role) const override { return role == role_to_find; }
};

class ConfigOptionFloatOrPercent;
class SimplifyVisitor {
    ArcFittingType                    m_use_arc_fitting;
    bool                              m_ignore_holes;
    coordf_t                          m_scaled_resolution;
    const ConfigOptionFloatOrPercent* m_arc_fitting_tolearance;
    const ExtrusionAttributes*         m_current_attributes = nullptr;
    // when an entity is too small, this is set to true do the collection that is higher in the stack can merge & delete.
    coord_t                           m_min_path_size = 0;
    bool                              m_last_deleted = false;
    void simplify_entity(ExtrusionEntity &entity);
public:
    SimplifyVisitor(coordf_t scaled_resolution, ArcFittingType use_arc_fitting, bool ignore_holes, const ConfigOptionFloatOrPercent *arc_fitting_tolearance)
        : m_scaled_resolution(scaled_resolution), m_ignore_holes(ignore_holes), m_use_arc_fitting(use_arc_fitting), m_arc_fitting_tolearance(arc_fitting_tolearance)
    {}
    SimplifyVisitor(coordf_t scaled_resolution, ArcFittingType use_arc_fitting, bool ignore_holes, const ConfigOptionFloatOrPercent *arc_fitting_tolearance, coord_t min_path_size)
        : m_scaled_resolution(scaled_resolution), m_ignore_holes(ignore_holes), m_use_arc_fitting(use_arc_fitting), m_arc_fitting_tolearance(arc_fitting_tolearance), m_min_path_size(min_path_size)
    {}

    static void simplify(ExtrusionEntity &entity, coordf_t tolerance, ArcFittingType with_fitting_arc, double fitting_arc_tolerance);
    void traverse(ExtrusionEntity &entity);
    bool is_valid() { return !m_last_deleted; }
};
class GetPathsVisitor : public ExtrusionTreeVisitor<false> {
public:
    std::vector<ExtrusionEntity*> paths;
    virtual void visit_leaf(ExtrusionEntity& entity) override;
};

class ExtrusionVolume : public ExtrusionTreeConstVisitor<false> {
    bool _with_gap_fill = true;
    double _flow_ratio = 1.;
public:
    double volume = 0; //unscaled
    ExtrusionVolume() {}
    void set_use_gap_fill(bool with_gap_fill = true) { _with_gap_fill = (with_gap_fill); }
    void set_flow_mult(double mult) { _flow_ratio = (mult); }
    void visit_leaf(const ExtrusionEntity &entity) override;
    double get(const ExtrusionEntityCollection &coll);
};

class ExtrusionModifyFlow : public ExtrusionTreeVisitor<false> {
    double _flow_mult = 1.;
public:
    ExtrusionModifyFlow(double flow_mult) : _flow_mult(flow_mult) {}
    void visit_leaf(ExtrusionEntity &entity) override;
    void set(ExtrusionEntityCollection &coll);
};

class CreateBoundingBoxVisitor : public ExtrusionTreeConstVisitor<false> {
    BoundingBox bb;
public:
    CreateBoundingBoxVisitor() {}
    void visit_leaf(const ExtrusionEntity &entity) override;
    static inline BoundingBox create(const ExtrusionEntity &ee) {
        CreateBoundingBoxVisitor visitor;
        visitor.traverse(ee);
        return visitor.bb;
    }
};

class CountEntities : public ExtrusionVisitorConst {
public:
    size_t count(const ExtrusionEntity &coll) { coll.visit(*this); return leaf_number; }
    size_t leaf_number = 0;
    virtual void default_use(const ExtrusionEntity &entity) override;
};

class FlatenEntities : public ExtrusionVisitorConst {
    ExtrusionEntityCollection to_fill;
    bool preserve_ordering;
public:
    using ExtrusionVisitorConst::use;
    FlatenEntities(bool preserve_ordering) : preserve_ordering(preserve_ordering) {}
    FlatenEntities(ExtrusionEntityCollection pattern, bool preserve_ordering) : preserve_ordering(preserve_ordering) {
        to_fill.set_can_sort_reverse(pattern.can_sort(), pattern.can_reverse());
    }
    FlatenEntities(const ExtrusionEntity &pattern, bool preserve_ordering) : preserve_ordering(preserve_ordering) {
        to_fill.set_can_sort_reverse(pattern.can_sort(), pattern.can_reverse());
    }
    const ExtrusionEntityCollection& get() {
        return to_fill;
    };
    ExtrusionEntityCollection& set() {
        return to_fill;
    };
    ExtrusionEntityCollection&& flatten(const ExtrusionEntityCollection &to_flatten) &&;
    void default_use(const ExtrusionEntity &entity) override;
};

#ifdef _DEBUG
class TestCollection : public ExtrusionVisitorRecursiveConst {
public:
    virtual void default_use(const ExtrusionEntity& entity) override;
};
#endif

#ifdef _DEBUGINFO
struct LoopAssertVisitor : public ExtrusionTreeConstVisitor<true> {
    coord_t m_check_length;
    LoopAssertVisitor() : m_check_length(SCALED_EPSILON) {}
    LoopAssertVisitor(coord_t check_length) : m_check_length(check_length) {}
protected:
    virtual void enter_node(const ExtrusionEntity& entity) override;
    virtual void visit_leaf(const ExtrusionEntity& entity) override;
};
#define DEBUGINFO_VISIT(ENTITY,VISITOR) (ENTITY).visit(VISITOR);
#endif

#ifdef _DEBUG
#define DEBUG_VISIT(ENTITY,VISITOR) (ENTITY).visit(VISITOR);
#define DEBUG_TREE_VISIT(ENTITY,VISITOR) (VISITOR).traverse(ENTITY);
#else
#define DEBUG_VISIT(ENTITY,VISITOR)
#define DEBUG_TREE_VISIT(ENTITY,VISITOR)
#endif

}

#endif
