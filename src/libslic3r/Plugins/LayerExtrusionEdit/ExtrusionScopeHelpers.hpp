///|/ Copyright (c) SuperSlicer 2026 Durand Remi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/

#ifndef slic3r_Plugins_LayerExtrusionEdit_ExtrusionScopeHelpers_hpp_
#define slic3r_Plugins_LayerExtrusionEdit_ExtrusionScopeHelpers_hpp_

/*
Compact ordered extrusion-scope helpers
=======================================

A scope keeps printable content in machine order and reserves only the phases
needed by real transitions. The possible child layouts are:

    content
    content, after
    travel, before, content
    travel, before, content, after

The stable root always carries PrintingExtrusionScopeProperty. When there are
no phases, the root is also the content. Otherwise the previous root content,
properties and auxiliary resources are moved into the named content child.

How producers and consumers use this module
-------------------------------------------

The scope producer calls ensure_scope(root, key, flags) once the transitions
around a candidate are known. The function preserves the root handle and
creates only the children required by the flags.

Later plugins receive that marked root through PrintingEntityPropertyTraversal
and construct OrderedExtrusionScope(root, key). They ask for named phases
instead of reproducing the conditional child indexes. For example, a travel
provider writes into travel(), a retraction provider may write into after() of
the source and before() of the target, and every plugin reads printable paths
through content().

Phase views are borrowed. A caller may edit a returned phase subtree, but must
not restructure the scope root while retaining any of its child views.
*/

#include "libslic3r/Api/plugin/cpp/ExtrusionViews.hpp"
#include "libslic3r/Plugins/PrintingPlan/PrintingExtrusionScopeProperty.h"

namespace slic3r_api { namespace LayerExtrusionEdit { namespace ExtrusionScope {

/*
Named borrowed views over one compact scope root.

Phase methods return an invalid view when that phase is not part of the shape.
Do not retain child views across structural edits of root.
*/
class OrderedExtrusionScope
{
public:
    /* Validate and open one already marked scope root. */
    OrderedExtrusionScope(
        MutableExtrusionEntity root,
        PluginPropertyKey<PrintingExtrusionScopeProperty> key);

    /* Return the stable marked root from which this view was constructed. */
    MutableExtrusionEntity root() const { return m_root; }

    /* Return whether the borrowed root handle is valid. */
    bool valid() const { return m_root.valid(); }

    /* Return whether travel() and before() physically exist. */
    bool has_incoming_transition() const;

    /* Return whether after() physically exists. */
    bool has_outgoing_transition() const;

    /* Return whether the incoming boundary includes a tool selection. */
    bool has_incoming_toolchange() const;

    /* Return whether the outgoing boundary includes a tool selection. */
    bool has_outgoing_toolchange() const;

    /* Return the incoming travel slot, or an invalid view when absent. */
    MutableExtrusionEntity travel() const;

    /* Return the ordered pre-content phase, or an invalid view when absent. */
    MutableExtrusionEntity before() const;

    /* Return printable content; this may be the scope root itself. */
    MutableExtrusionEntity content() const;

    /* Return the ordered post-content phase, or an invalid view when absent. */
    MutableExtrusionEntity after() const;

private:
    /* Read the marker again so structural mutations cannot leave a stale pointer. */
    const PrintingExtrusionScopeProperty &property() const;

    /* Borrowed stable root owned by its PrintingExtrusion. */
    MutableExtrusionEntity m_root;

    /* Typed runtime identity used to reacquire the marker. */
    PluginPropertyKey<PrintingExtrusionScopeProperty> m_key;
};

/*
Return true when entity carries a marker and its shape matches its flags.

Absence is a normal false result. A present marker with an invalid shape is a
broken private contract and therefore raises an exception.
*/
bool is_scope(
    const ExtrusionEntity &entity,
    const PluginPropertyKey<PrintingExtrusionScopeProperty> &key);

/*
Create or validate one compact scope without replacing its stable root handle.

Existing content is moved only when a phase is required. Calling the function
again with the same flags returns the existing scope. Contradictory flags or an
existing malformed shape are reported explicitly.
*/
OrderedExtrusionScope ensure_scope(
    MutableExtrusionEntity entity,
    const PluginPropertyKey<PrintingExtrusionScopeProperty> &key,
    uint8_t flags);

/*
Append one empty leaf after the existing contents of a reserved phase.

The first producer may use the empty phase root directly. Later producers are
placed after it without moving properties away from existing events. This is
the common insertion primitive for ordered Retract, Wipe and Unretract events.
*/
MutableExtrusionEntity append_phase_leaf(MutableExtrusionEntity phase);

}}} // namespace slic3r_api::LayerExtrusionEdit::ExtrusionScope

#endif // slic3r_Plugins_LayerExtrusionEdit_ExtrusionScopeHelpers_hpp_
