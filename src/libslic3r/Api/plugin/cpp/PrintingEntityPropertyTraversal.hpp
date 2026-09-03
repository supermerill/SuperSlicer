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
order. Only entities carrying the requested direct property participate. An
optional state definition can copy selected inherited properties into each
match; those values are resolved lazily only on branches which produce one.

The callback receives consecutive matching entities. Null pointers mark the
beginning and end of this layer-local sequence. Context objects and their
property pointers are borrowed and exist only for the duration of the callback;
plugins must not retain them.

Developer guide: [Using Unified Extrusion Entities](/doc/plugins/extrusions.md)
*/

#include <any>
#include <functional>
#include <optional>
#include <stdexcept>
#include <typeindex>
#include <type_traits>
#include <utility>
#include <vector>

#include "libslic3r/Api/plugin/cpp/PluginPropertyKey.hpp"
#include "libslic3r/Api/plugin/cpp/PrintingPlanViews.hpp"

namespace slic3r_api {

class ExtrusionPropertyState;

/*
Describe the inherited extrusion properties needed by one traversal.

Calling track() does not read an extrusion tree. It records a typed key and
the rule used later to combine direct values along a root-to-match path. Keep
the definition local to the traversal which consumes it: dynamic property ids
belong to the orchestrator which created their PluginPropertyKey.

Only autonomous, trivially copyable payloads may be tracked. In particular, a
payload containing an extrusion_data_id is not autonomous because the copied
id still refers to storage owned by an extrusion tree.
*/
class ExtrusionPropertyStateDefinition
{
public:
    /*
    Track one property with ordinary inheritance semantics.

    The nearest direct value replaces the complete effective payload. A key
    may be registered only once in this definition.
    */
    template<class Property>
    ExtrusionPropertyStateDefinition &track(
        const PluginPropertyKey<Property> &key)
    {
        add_rule<Property>(key, std::function<void(Property &, const Property &)>());
        return *this;
    }

    /*
    Track one property with a field-aware inheritance rule.

    The first direct value is copied without invoking merge. For every deeper
    direct value, merge(effective, direct) updates the accumulated payload.
    This supports properties where only positive or otherwise selected fields
    override inherited fields.
    */
    template<class Property, class Merge>
    ExtrusionPropertyStateDefinition &track(
        const PluginPropertyKey<Property> &key,
        Merge merge)
    {
        static_assert(
            std::is_invocable_r<void, Merge &, Property &, const Property &>::value,
            "An extrusion property merge must accept (Property &, const Property &)."
        );
        std::function<void(Property &, const Property &)> callback(std::move(merge));
        if (!callback)
            throw std::invalid_argument(
                "An extrusion property state needs a valid merge callback.");
        add_rule<Property>(key, std::move(callback));
        return *this;
    }

private:
    /* One type-erased rule identifies and merges a single typed payload. */
    struct Rule
    {
        orchestrator_handle *orchestrator = nullptr;
        slic3r_property_type type = SLIC3R_PROPERTY_TYPE_INVALID;
        std::type_index payload_type{typeid(void)};
        std::function<void(ExtrusionPropertyState &, const ExtrusionEntity &)> apply;
    };

    /* Validate one key and append its typed direct-property reader. */
    template<class Property>
    void add_rule(
        const PluginPropertyKey<Property> &key,
        std::function<void(Property &, const Property &)> merge);

    // Ordered rules; their order also defines deterministic snapshot storage.
    std::vector<Rule> m_rules;

    template<class Property> friend class PrintingEntityPropertyTraversal;
};

/*
Own the effective values requested by ExtrusionPropertyStateDefinition.

Every stored payload is a value copy. A snapshot therefore remains valid when
the traversal leaves its ancestor branch and may safely be copied into the
`previous` callback argument. get() returns a pointer into this snapshot, not
into the extrusion tree; the pointer remains valid until the snapshot is
modified, assigned or destroyed.
*/
class ExtrusionPropertyState
{
public:
    /*
    Return the effective tracked payload, or nullptr when no direct value was
    found from the PrintingExtrusion root through the matching entity.

    Asking for a different C++ payload type with the same runtime key is a
    programming error and throws std::logic_error.
    */
    template<class Property>
    const Property *get(const PluginPropertyKey<Property> &key) const
    {
        for (const Entry &entry : m_entries) {
            if (entry.orchestrator != key.orchestrator() || entry.type != key.type())
                continue;
            if (entry.payload_type != std::type_index(typeid(Property)))
                throw std::logic_error(
                    "An inherited extrusion property was requested with another payload type.");
            return std::any_cast<Property>(&entry.value);
        }
        return nullptr;
    }

private:
    /* One autonomous payload copy, identified exactly like PluginPropertyKey. */
    struct Entry
    {
        // Dynamic property ids are allocated per orchestrator. Retaining the
        // borrowed orchestrator distinguishes equal numeric ids originating
        // from different registries; built-in properties use nullptr.
        orchestrator_handle *orchestrator = nullptr;
        slic3r_property_type type = SLIC3R_PROPERTY_TYPE_INVALID;
        std::type_index payload_type{typeid(void)};
        std::any value;
    };

    /* Copy or merge a direct payload into the effective snapshot. */
    template<class Property>
    void merge_direct(
        const PluginPropertyKey<Property> &key,
        const Property &direct,
        const std::function<void(Property &, const Property &)> &merge)
    {
        for (Entry &entry : m_entries) {
            if (entry.orchestrator != key.orchestrator() || entry.type != key.type())
                continue;
            if (entry.payload_type != std::type_index(typeid(Property)))
                throw std::logic_error(
                    "An inherited extrusion property has an inconsistent payload type.");
            Property *effective = std::any_cast<Property>(&entry.value);
            if (merge)
                merge(*effective, direct);
            else
                *effective = direct;
            return;
        }

        Entry entry;
        entry.orchestrator = key.orchestrator();
        entry.type = key.type();
        entry.payload_type = std::type_index(typeid(Property));
        entry.value = direct;
        m_entries.emplace_back(std::move(entry));
    }

    // Present effective payloads only; absent tracked properties use no entry.
    std::vector<Entry> m_entries;

    friend class ExtrusionPropertyStateDefinition;
};

template<class Property>
void ExtrusionPropertyStateDefinition::add_rule(
    const PluginPropertyKey<Property> &key,
    std::function<void(Property &, const Property &)> merge)
{
    static_assert(std::is_trivially_copyable<Property>::value,
                  "Inherited extrusion property snapshots require trivial payloads.");
    static_assert(!std::is_same<Property, EPropertyCustomGcode>::value,
                  "Properties owning extrusion_data_id resources cannot be snapshotted.");
    if (key.type() == SLIC3R_PROPERTY_TYPE_INVALID)
        throw std::invalid_argument(
            "An extrusion property state cannot track an invalid key.");
    for (const Rule &rule : m_rules) {
        if (rule.orchestrator == key.orchestrator() && rule.type == key.type())
            throw std::invalid_argument(
                "An extrusion property state cannot track the same key twice.");
    }

    Rule rule;
    rule.orchestrator = key.orchestrator();
    rule.type = key.type();
    rule.payload_type = std::type_index(typeid(Property));
    rule.apply = [key, merge = std::move(merge)](
        ExtrusionPropertyState &state, const ExtrusionEntity &entity) {
        const Property *direct = entity.get(key);
        if (direct != nullptr)
            state.merge_direct(key, *direct, merge);
    };
    m_rules.emplace_back(std::move(rule));
}

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
    // Autonomous root-to-entity snapshot requested by the traversal definition.
    ExtrusionPropertyState effective_properties;
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
        validate_arguments();
    }

    /*
    Store an inherited-state definition in addition to the selecting key.

    State is still computed only when a matching entity is reached. Branches
    with no match do not read or merge tracked properties. The snapshot passed
    to a callback includes direct values on the matching entity itself.
    */
    PrintingEntityPropertyTraversal(
        PluginPropertyKey<Property> key,
        ExtrusionPropertyStateDefinition state_definition,
        ProcessEntity process_entity,
        MatchingEntityDescendants matching_entity_descendants =
            MatchingEntityDescendants::Visit) :
        m_key(std::move(key)),
        m_state_definition(state_definition.m_rules.empty() ?
            std::optional<ExtrusionPropertyStateDefinition>() :
            std::optional<ExtrusionPropertyStateDefinition>(
                std::move(state_definition))),
        m_process_entity(std::move(process_entity)),
        m_matching_entity_descendants(matching_entity_descendants)
    {
        validate_arguments();
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
        if (m_state_definition) {
            std::vector<StateFrame> active_path;
            for (uint32_t tool_idx = 0; tool_idx < layer_group.tool_group_count(); ++tool_idx) {
                const PrintingToolGroup tool_group = layer_group.tool_group(tool_idx);
                for (uint32_t extrusion_idx = 0;
                     extrusion_idx < tool_group.extrusion_count(); ++extrusion_idx) {
                    const PrintingExtrusion printing_extrusion =
                        tool_group.extrusion(extrusion_idx);
                    process_tree_with_state(
                        layer_group, tool_group, printing_extrusion,
                        printing_extrusion.mutable_root(), previous, active_path);
                }
            }

            if (previous) {
                refresh_property(*previous);
                m_process_entity(&*previous, nullptr);
            }
            return;
        }

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
                layer_group, tool_group, printing_extrusion, entity, property,
                ExtrusionPropertyState{}
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
    One active root-to-current frame with an optional resolved snapshot.

    Parent frames stay alive while descendants are visited, allowing siblings
    to copy an already resolved parent without rebuilding it.
    */
    struct StateFrame
    {
        MutableExtrusionEntity entity;
        std::optional<ExtrusionPropertyState> effective_properties;
    };

    /*
    Visit one subtree while retaining only its active ancestor chain.

    Frames are pushed for every visited node, but their property state remains
    empty until this branch reaches a matching entity.
    */
    void process_tree_with_state(
        const PrintingLayerGroup &layer_group,
        const PrintingToolGroup &tool_group,
        const PrintingExtrusion &printing_extrusion,
        MutableExtrusionEntity entity,
        std::optional<Entity> &previous,
        std::vector<StateFrame> &active_path) const
    {
        active_path.push_back(StateFrame{entity, std::nullopt});
        Property *property = entity.get_mutable(m_key);
        if (property != nullptr) {
            Entity current{
                layer_group, tool_group, printing_extrusion, entity, property,
                resolve_active_state(active_path)
            };
            if (previous) {
                refresh_property(*previous);
                m_process_entity(&*previous, &current);
            } else {
                m_process_entity(nullptr, &current);
            }

            refresh_property(current);
            previous = current;
            if (m_matching_entity_descendants == MatchingEntityDescendants::Skip) {
                active_path.pop_back();
                return;
            }
        }

        const uint32_t child_count = entity.child_count();
        for (uint32_t child_idx = 0; child_idx < child_count; ++child_idx)
            process_tree_with_state(
                layer_group, tool_group, printing_extrusion,
                entity.child_mutable(child_idx), previous, active_path);
        active_path.pop_back();
    }

    /*
    Materialize only the unresolved suffix of the current ancestor path.

    The nearest resolved ancestor is copied, then each missing frame merges its
    direct properties in root-to-current order. This is the sole point where
    tracked property lookups and custom merge callbacks run.
    */
    ExtrusionPropertyState resolve_active_state(
        std::vector<StateFrame> &active_path) const
    {
        size_t first_unresolved = 0;
        ExtrusionPropertyState state;
        for (size_t idx = active_path.size(); idx > 0; --idx) {
            if (active_path[idx - 1].effective_properties) {
                state = *active_path[idx - 1].effective_properties;
                first_unresolved = idx;
                break;
            }
        }

        for (size_t idx = first_unresolved; idx < active_path.size(); ++idx) {
            for (const ExtrusionPropertyStateDefinition::Rule &rule :
                 m_state_definition->m_rules)
                rule.apply(state, active_path[idx].entity.readonly());
            active_path[idx].effective_properties = state;
        }
        return state;
    }

    /* Validate callback and descendant mode for both constructors. */
    void validate_arguments() const
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
    // Optional typed rules; absent for the allocation-free historical path.
    std::optional<ExtrusionPropertyStateDefinition> m_state_definition;
    // User callback invoked synchronously for layer-local sequence boundaries.
    ProcessEntity m_process_entity;
    // Controls whether descendants of an already matching entity are visited.
    MatchingEntityDescendants m_matching_entity_descendants;
};

} // namespace slic3r_api

#endif // slic3r_Api_plugin_cpp_PrintingEntityPropertyTraversal_hpp_
