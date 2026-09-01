///|/ Copyright (c) SuperSlicer 2026 Durand Remi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/

#ifndef slic3r_Api_plugin_cpp_PrintingEntityPropertyTraversal_hpp_
#define slic3r_Api_plugin_cpp_PrintingEntityPropertyTraversal_hpp_

/*
PrintingPlan property traversal
===============================

PrintingEntityPropertyTraversal visits one final ordered PrintingLayerGroup
without first copying its extrusion nodes into a temporary array. It walks
tool groups, PrintingExtrusions, and each extrusion tree in their current
order. Only entities carrying the requested direct property participate.

The callback receives consecutive matching entities. Null pointers mark the
beginning and end of this layer-local sequence. Context objects and their
property pointers are borrowed and exist only for the duration of the callback;
plugins must not retain them.

Developer guide: [Using Unified Extrusion Entities](../../../../../doc/plugins/extrusions.md)
*/

#include <functional>
#include <optional>
#include <stdexcept>
#include <utility>

#include "libslic3r/Api/plugin/cpp/PluginPropertyKey.hpp"
#include "libslic3r/Api/plugin/cpp/PrintingPlanViews.hpp"

namespace slic3r_api {

/*
Borrowed owner context for one matching extrusion entity.

The four views are small handle wrappers copied from the PrintingPlan. They do
not own their underlying objects. property points into entity and is refreshed
before every callback because another callback may have changed the entity's
property storage.
*/
template<class Property> struct PrintingEntity
{
    // Layer whose ordered contents are currently being traversed.
    PrintingLayerGroup layer_group;
    // Tool group which owns printing_extrusion.
    PrintingToolGroup tool_group;
    // PrintingPlan entry which owns the extrusion tree.
    PrintingExtrusion printing_extrusion;
    // Exact tree node carrying the requested direct property.
    MutableExtrusionEntity entity;
    // Mutable borrowed payload; valid only until this entity's properties change.
    Property *property;
};

/*
Choose whether a matching entity's descendants can contain another match.

Visit keeps the general depth-first traversal and reports direct properties on
both a parent and its descendants. Skip treats the first matching entity as the
owner of its complete logical subtree. Skip is useful for non-nested scope
roots and avoids visiting their potentially large content and phase trees.
*/
enum class MatchingEntityDescendants
{
    Visit,
    Skip
};

/*
Stream consecutive entities carrying one typed direct property.

For matching entities A, B, and C, process() invokes:

    callback(nullptr, &A);
    callback(&A, &B);
    callback(&B, &C);
    callback(&C, nullptr);

Each process() call is independent. A layer without a matching entity emits no
callback. The callback may modify payload values, add unrelated properties, and
edit a dedicated descendant subtree such as a transition scope's travel,
before, or after phase. These edits are safe because they keep the matching
scope root and its position in the outer traversal stable.

The callback must not change the child list of a matching entity or one of its
active ancestors, add the searched property to a new entity, remove it from a
matched entity, or reorder matching entities. Such changes would alter the set
or order being streamed. Context pointers and property pointers are borrowed
for the callback only and must not be retained.
*/
template<class Property> class PrintingEntityPropertyTraversal
{
public:
    using Entity = PrintingEntity<Property>;
    using ProcessEntity = std::function<void(Entity *previous, Entity *next)>;

    /*
    Store the typed property identity and the callback used for every boundary.

    A callback is mandatory because silently traversing the complete layer
    would hide a programming error from the plugin author. The descendant mode
    defaults to the general Visit behavior; select Skip only when a matching
    entity owns a subtree which cannot contain another relevant match.
    */
    PrintingEntityPropertyTraversal(
        PluginPropertyKey<Property> key,
        ProcessEntity process_entity,
        MatchingEntityDescendants matching_entity_descendants =
            MatchingEntityDescendants::Visit) :
        m_key(std::move(key)),
        m_process_entity(std::move(process_entity)),
        m_matching_entity_descendants(matching_entity_descendants)
    {
        if (!m_process_entity)
            throw std::invalid_argument(
                "PrintingEntityPropertyTraversal needs a process callback.");
        if (m_matching_entity_descendants != MatchingEntityDescendants::Visit &&
            m_matching_entity_descendants != MatchingEntityDescendants::Skip)
            throw std::invalid_argument(
                "PrintingEntityPropertyTraversal received an invalid descendant mode.");
    }

    /*
    Traverse one layer in machine order and emit its local matching sequence.

    The method does not preserve a previous entity between calls. Exceptions
    raised by the callback are deliberately propagated to the caller.
    */
    void process(const PrintingLayerGroup &layer_group) const
    {
        if (!layer_group.valid())
            throw std::invalid_argument(
                "PrintingEntityPropertyTraversal needs a valid layer group.");

        std::optional<Entity> previous;
        for (uint32_t tool_idx = 0; tool_idx < layer_group.tool_group_count(); ++tool_idx) {
            const PrintingToolGroup tool_group = layer_group.tool_group(tool_idx);
            for (uint32_t extrusion_idx = 0;
                 extrusion_idx < tool_group.extrusion_count(); ++extrusion_idx) {
                const PrintingExtrusion printing_extrusion =
                    tool_group.extrusion(extrusion_idx);
                process_tree(layer_group, tool_group, printing_extrusion,
                             printing_extrusion.mutable_root(), previous);
            }
        }

        if (previous) {
            refresh_property(*previous);
            m_process_entity(&*previous, nullptr);
        }
    }

private:
    /*
    Visit one extrusion subtree in depth-first pre-order.

    previous is the sole traversal state. Keeping only this context avoids a
    temporary vector while still allowing a callback to compare two neighbors.
    */
    void process_tree(const PrintingLayerGroup &layer_group,
                      const PrintingToolGroup &tool_group,
                      const PrintingExtrusion &printing_extrusion,
                      MutableExtrusionEntity entity,
                      std::optional<Entity> &previous) const
    {
        Property *property = entity.get_mutable(m_key);
        if (property != nullptr) {
            Entity current{
                layer_group, tool_group, printing_extrusion, entity, property
            };
            if (previous) {
                refresh_property(*previous);
                m_process_entity(&*previous, &current);
            } else {
                m_process_entity(nullptr, &current);
            }

            refresh_property(current);
            previous = current;

            // A scope marker describes the whole subtree, so its phases and
            // printable content cannot contain another relevant scope root.
            if (m_matching_entity_descendants == MatchingEntityDescendants::Skip)
                return;
        }

        const uint32_t child_count = entity.child_count();
        for (uint32_t child_idx = 0; child_idx < child_count; ++child_idx)
            process_tree(layer_group, tool_group, printing_extrusion,
                         entity.child_mutable(child_idx), previous);
    }

    /*
    Reacquire the borrowed payload before exposing a retained neighbor again.

    Removing the searched property during traversal violates the traversal
    contract and is reported immediately instead of returning a stale pointer.
    */
    void refresh_property(Entity &context) const
    {
        context.property = context.entity.get_mutable(m_key);
        if (context.property == nullptr)
            throw std::runtime_error(
                "A traversed entity lost the property used to select it.");
    }

    // Typed direct property used to select entities and refresh their payload.
    PluginPropertyKey<Property> m_key;
    // User callback invoked synchronously for layer-local sequence boundaries.
    ProcessEntity m_process_entity;
    // Controls whether descendants of an already matching entity are visited.
    MatchingEntityDescendants m_matching_entity_descendants;
};

} // namespace slic3r_api

#endif // slic3r_Api_plugin_cpp_PrintingEntityPropertyTraversal_hpp_
