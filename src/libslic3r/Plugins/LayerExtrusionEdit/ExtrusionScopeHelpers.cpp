///|/ Copyright (c) SuperSlicer 2026 Durand Remi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/

#include "ExtrusionScopeHelpers.hpp"

#include <stdexcept>

/*
Compact ordered extrusion-scope implementation
==============================================

The ordered-leaf primitive preserves the outer entity handle while moving old
content and direct properties into a child. Building phases from the right and
then prepending entry phases produces every supported compact layout without a
second tree-editing mechanism.
*/

namespace slic3r_api { namespace LayerExtrusionEdit { namespace ExtrusionScope {
namespace {

/* Return the child count dictated by incoming and outgoing transition flags. */
uint32_t expected_child_count(uint8_t flags);

/* Reject unknown or internally contradictory scope flags. */
void validate_flags(uint8_t flags);

/* Validate ordering permissions and the child layout of a marked scope. */
void validate_shape(
    const ExtrusionEntity &entity,
    const PrintingExtrusionScopeProperty &property);

/* Derive the exact root child count from incoming and outgoing phase bits. */
uint32_t expected_child_count(const uint8_t flags)
{
    // The layouts are intentionally derived from only the two transition
    // bits. START, TERMINAL and materialized-travel facts never add children.
    const bool incoming =
        (flags & PRINTING_EXTRUSION_SCOPE_INCOMING_TRANSITION) != 0;
    const bool outgoing =
        (flags & PRINTING_EXTRUSION_SCOPE_OUTGOING_TRANSITION) != 0;
    if (incoming && outgoing)
        return 4;
    if (incoming)
        return 3;
    if (outgoing)
        return 2;
    return 0;
}

/* Verify that flags describe a representable compact scope. */
void validate_flags(const uint8_t flags)
{
    // Reject unknown bits before inspecting relationships between known bits;
    // this catches a producer compiled against an incompatible private layout.
    constexpr uint8_t known_flags =
        PRINTING_EXTRUSION_SCOPE_INCOMING_TRANSITION |
        PRINTING_EXTRUSION_SCOPE_OUTGOING_TRANSITION |
        PRINTING_EXTRUSION_SCOPE_INCOMING_TRAVEL_MATERIALIZED |
        PRINTING_EXTRUSION_SCOPE_OUTGOING_TRAVEL_MATERIALIZED |
        PRINTING_EXTRUSION_SCOPE_START |
        PRINTING_EXTRUSION_SCOPE_TERMINAL;
    if ((flags & ~known_flags) != 0)
        throw std::invalid_argument("An extrusion scope contains unknown flags.");

    // A materialized travel is information about a real transition. Without
    // the matching transition there is no adjacent boundary to describe.
    if ((flags & PRINTING_EXTRUSION_SCOPE_INCOMING_TRAVEL_MATERIALIZED) != 0 &&
        (flags & PRINTING_EXTRUSION_SCOPE_INCOMING_TRANSITION) == 0)
        throw std::invalid_argument(
            "An incoming materialized travel needs an incoming transition.");
    if ((flags & PRINTING_EXTRUSION_SCOPE_OUTGOING_TRAVEL_MATERIALIZED) != 0 &&
        (flags & PRINTING_EXTRUSION_SCOPE_OUTGOING_TRANSITION) == 0)
        throw std::invalid_argument(
            "An outgoing materialized travel needs an outgoing transition.");
}

/* Verify that one marked root physically matches the phases in its marker. */
void validate_shape(
    const ExtrusionEntity &entity,
    const PrintingExtrusionScopeProperty &property)
{
    validate_flags(property.flags);

    // Process plugins rely on the order travel -> before -> content -> after.
    // A sortable or reversible root could silently invalidate that contract.
    if (entity.sortable() || entity.reversible())
        throw std::runtime_error(
            "An extrusion scope root must be non-sortable and non-reversible.");

    const uint32_t expected = expected_child_count(property.flags);
    if (expected == 0) {
        // A phase-less scope is its own content and may therefore be either a
        // geometric leaf or an existing continuous collection.
        return;
    }
    if (entity.child_count() != expected)
        throw std::runtime_error(
            "An extrusion scope shape contradicts its transition flags.");
}

} // namespace

/* Open a marked root and reject malformed producer output immediately. */
OrderedExtrusionScope::OrderedExtrusionScope(
    MutableExtrusionEntity root,
    PluginPropertyKey<PrintingExtrusionScopeProperty> key) :
    m_root(root), m_key(key)
{
    if (!m_root.valid())
        throw std::invalid_argument("OrderedExtrusionScope needs a valid root.");
    validate_shape(m_root.readonly(), property());
}

/* Reacquire the marker instead of retaining a pointer across tree edits. */
const PrintingExtrusionScopeProperty &OrderedExtrusionScope::property() const
{
    const PrintingExtrusionScopeProperty *value = m_root.get(m_key);
    if (value == nullptr)
        throw std::runtime_error("An ordered extrusion scope lost its marker.");
    return *value;
}

/* Report whether this scope owns the incoming travel and before phases. */
bool OrderedExtrusionScope::has_incoming_transition() const
{
    return printing_extrusion_scope_has_flag(
        property(), PRINTING_EXTRUSION_SCOPE_INCOMING_TRANSITION);
}

/* Report whether this scope owns an outgoing after phase. */
bool OrderedExtrusionScope::has_outgoing_transition() const
{
    return printing_extrusion_scope_has_flag(
        property(), PRINTING_EXTRUSION_SCOPE_OUTGOING_TRANSITION);
}

/* Resolve the first child of an incoming-transition layout. */
MutableExtrusionEntity OrderedExtrusionScope::travel() const
{
    return has_incoming_transition() ? m_root.child_mutable(0) :
                                       MutableExtrusionEntity();
}

/* Resolve the second child of an incoming-transition layout. */
MutableExtrusionEntity OrderedExtrusionScope::before() const
{
    return has_incoming_transition() ? m_root.child_mutable(1) :
                                       MutableExtrusionEntity();
}

/* Resolve content for each of the four supported compact layouts. */
MutableExtrusionEntity OrderedExtrusionScope::content() const
{
    return has_incoming_transition() ? m_root.child_mutable(2) :
        (has_outgoing_transition() ? m_root.child_mutable(0) : m_root);
}

/* Resolve the final child of an outgoing-transition layout. */
MutableExtrusionEntity OrderedExtrusionScope::after() const
{
    if (!has_outgoing_transition())
        return MutableExtrusionEntity();
    return m_root.child_mutable(has_incoming_transition() ? 3 : 1);
}

/* Detect a scope marker and validate the associated structural contract. */
bool is_scope(
    const ExtrusionEntity &entity,
    const PluginPropertyKey<PrintingExtrusionScopeProperty> &key)
{
    const PrintingExtrusionScopeProperty *property = entity.get(key);
    if (property == nullptr)
        return false;
    validate_shape(entity, *property);
    return true;
}

/*
Create the conditional phase layout while preserving the supplied root handle.

The operation proceeds from the content side outward. An outgoing phase first
moves the old root into content and appends after. Incoming phases are then
prepended as before and travel. This ordering lets emplace_ordered_leaf() carry
properties and variable resources with the old content exactly once.
*/
OrderedExtrusionScope ensure_scope(
    MutableExtrusionEntity entity,
    const PluginPropertyKey<PrintingExtrusionScopeProperty> &key,
    const uint8_t flags)
{
    if (!entity.valid())
        throw std::invalid_argument("A compact extrusion scope needs a valid entity.");
    validate_flags(flags);

    // A repeated producer run must validate and reuse its root. Rebuilding an
    // existing scope would create nested wrappers and invalidate phase views.
    if (const PrintingExtrusionScopeProperty *existing = entity.get(key)) {
        if (existing->flags != flags)
            throw std::runtime_error(
                "An existing extrusion scope has different transition flags.");
        validate_shape(entity.readonly(), *existing);
        return OrderedExtrusionScope(entity, key);
    }

    const bool incoming =
        (flags & PRINTING_EXTRUSION_SCOPE_INCOMING_TRANSITION) != 0;
    const bool outgoing =
        (flags & PRINTING_EXTRUSION_SCOPE_OUTGOING_TRANSITION) != 0;

    if (!incoming && !outgoing) {
        // With no transition phases the candidate itself is the content. It
        // still becomes fixed so later plugins may trust the execution order.
        entity.disable_sort().disable_reverse();
    } else {
        // Moving the old content while adding the first phase keeps all direct
        // print properties and their variable resources on content.
        if (outgoing) {
            if (!entity.emplace_ordered_leaf(
                    OrderedLeafPosition::After,
                    ExistingPropertyPlacement::MoveWithExistingContent).valid())
                throw std::runtime_error("Unable to create an extrusion scope after phase.");
        }
        if (incoming) {
            // When after already moved the old content into a child, prepending
            // before must keep that child tree intact. Otherwise this first
            // prepend performs the one required content/property move.
            const ExistingPropertyPlacement placement = outgoing ?
                ExistingPropertyPlacement::KeepOnParent :
                ExistingPropertyPlacement::MoveWithExistingContent;
            if (!entity.emplace_ordered_leaf(
                    OrderedLeafPosition::Before, placement).valid())
                throw std::runtime_error("Unable to create an extrusion scope before phase.");

            // Travel is always the first phase and is deliberately separate
            // from before, which later holds tool changes or unretraction.
            if (!entity.emplace_ordered_leaf(
                    OrderedLeafPosition::Before,
                    ExistingPropertyPlacement::KeepOnParent).valid())
                throw std::runtime_error("Unable to create an extrusion scope travel phase.");
        }
    }

    // Attach the marker last. If phase creation fails, callers never observe a
    // root claiming to be a complete scope with an incomplete shape.
    PrintingExtrusionScopeProperty &property = entity.get_or_add(key);
    property.flags = flags;
    return OrderedExtrusionScope(entity, key);
}

}}} // namespace slic3r_api::LayerExtrusionEdit::ExtrusionScope
