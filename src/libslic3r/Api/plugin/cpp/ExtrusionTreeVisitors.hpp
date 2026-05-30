///|/ Copyright (c) SuperSlicer 2026 Durand Rémi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/
#ifndef slic3r_Api_plugin_cpp_ExtrusionTreeVisitors_hpp_
#define slic3r_Api_plugin_cpp_ExtrusionTreeVisitors_hpp_

#include <cassert>
#include <cstdint>
#include <vector>

#include "libslic3r/Api/plugin/cpp/ExtrusionViews.hpp"

namespace slic3r_api {

/*
Extrusion tree visitors
=======================

Plugin code often receives one extrusion root from a LayerRegionIsland and then
needs to inspect or edit every printable leaf. The tree is small enough to look
simple, but writing the traversal by hand in every plugin is error-prone:

- properties such as EPropertyAttributes may be inherited from a parent node;
- a leaf may be replaced by a collection while it is being processed;
- recursive functions are easy to copy incorrectly and can grow the C++ stack
  on pathological extrusion trees.

These visitors provide the same "enter / leaf / leave" model used by the host
ExtrusionTreeVisitor, but they work only with the plugin ABI views:

    class MyVisitor : public ExtrusionTreeConstVisitor<> {
        void visit_leaf(ExtrusionEntity entity) override {
            if (const EPropertyAttributes *attr = current_property<EPropertyAttributes>())
                ...
        }
    };

    MyVisitor visitor;
    visitor.traverse(root);

Use ExtrusionTreeConstVisitor for read-only algorithms and ExtrusionTreeVisitor
for algorithms that edit the borrowed mutable tree.

Mutation rules
--------------

The mutable visitor is designed for local edits on the entity currently passed
to enter_node(), visit_leaf(), or leave_node(). Replacing the current leaf by a
collection is allowed; the new children are not visited during this traversal.

Avoid inserting/removing siblings or ancestors while the traversal is active.
Those operations can invalidate child order assumptions and may invalidate
borrowed handles depending on the host storage. If a plugin needs large tree
rewrites, first collect the target handles or ranges, then rewrite them in a
separate pass.

Property lookup
---------------

current_property<T>() searches the active stack from the current entity toward
the root and returns the first direct property it finds. The returned pointer is
borrowed from the entity that owns the property. Treat it as read-only and do
not keep it after mutating the tree.
*/

namespace detail {

inline ExtrusionEntity extrusion_tree_child(const ExtrusionEntity &entity, uint32_t idx)
{
    return entity.child(idx);
}

inline MutableExtrusionEntity extrusion_tree_child(const MutableExtrusionEntity &entity, uint32_t idx)
{
    return entity.child_mutable(idx);
}

} // namespace detail

template<class Entity, bool LeafIsNode = true>
class ExtrusionTreeVisitorBase
{
public:
    virtual ~ExtrusionTreeVisitorBase() = default;

    void traverse(Entity root)
    {
        m_stack.clear();
        m_stack.push_back({ root });

        while (!m_stack.empty()) {
            Frame &frame = m_stack.back();
            Entity entity = frame.entity;

            if (!frame.entered) {
                if constexpr (LeafIsNode) {
                    enter_node(entity);
                } else if (entity.child_count() > 0) {
                    enter_node(entity);
                }
                frame.entered = true;
            }

            if (frame.next_child < entity.child_count()) {
                m_stack.push_back({ detail::extrusion_tree_child(entity, frame.next_child++) });
                continue;
            }

            if (entity.child_count() == 0) {
                visit_leaf(entity);
                if constexpr (LeafIsNode)
                    leave_node(entity);
            } else {
                leave_node(entity);
            }

            m_stack.pop_back();
        }
    }

protected:
    virtual void enter_node(Entity entity) {}
    virtual void visit_leaf(Entity entity) {}
    virtual void leave_node(Entity entity) {}

    uint32_t depth() const
    {
        assert(!m_stack.empty());
        return uint32_t(m_stack.size() - 1);
    }

    template<class PropertyType>
    const PropertyType *current_property() const
    {
        for (typename std::vector<Frame>::const_reverse_iterator it = m_stack.rbegin();
             it != m_stack.rend(); ++it) {
            if (const PropertyType *property = it->entity.template property<PropertyType>())
                return property;
        }
        return nullptr;
    }

private:
    struct Frame
    {
        Entity entity;
        uint32_t next_child = 0;
        bool entered = false;
    };

    std::vector<Frame> m_stack;
};

template<bool LeafIsNode = true>
class ExtrusionTreeConstVisitor : public ExtrusionTreeVisitorBase<ExtrusionEntity, LeafIsNode>
{
};

template<bool LeafIsNode = true>
class ExtrusionTreeVisitor : public ExtrusionTreeVisitorBase<MutableExtrusionEntity, LeafIsNode>
{
};

} // namespace slic3r_api

#endif // slic3r_Api_plugin_cpp_ExtrusionTreeVisitors_hpp_
