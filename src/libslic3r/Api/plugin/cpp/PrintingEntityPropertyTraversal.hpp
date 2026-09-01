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
Stream consecutive entities carrying one typed direct property.

For matching entities A, B, and C, process() invokes:

    callback(nullptr, &A);
    callback(&A, &B);
    callback(&B, &C);
    callback(&C, nullptr);

Each process() call is independent. A layer without a matching entity emits no
callback. The callback may modify payload values and unrelated descendants,
but it must not remove the searched property or add, remove, or reorder nodes
in the hierarchy currently being traversed.
*/
template<class Property> class PrintingEntityPropertyTraversal
{
public:
    using Entity = PrintingEntity<Property>;
    using ProcessEntity = std::function<void(Entity *previous, Entity *next)>;

    /*
    Store the typed property identity and the callback used for every boundary.

    A callback is mandatory because silently traversing the complete layer
    would hide a programming error from the plugin author.
    */
    PrintingEntityPropertyTraversal(PluginPropertyKey<Property> key,
                                    ProcessEntity process_entity) :
        m_key(std::move(key)), m_process_entity(std::move(process_entity))
    {
        if (!m_process_entity)
            throw std::invalid_argument(
                "PrintingEntityPropertyTraversal needs a process callback.");
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
};

} // namespace slic3r_api

#endif // slic3r_Api_plugin_cpp_PrintingEntityPropertyTraversal_hpp_
