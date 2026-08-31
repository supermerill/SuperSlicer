///|/ Copyright (c) Prusa Research 2016 - 2023 Vojtěch Bubník @bubnikv, Lukáš Matěna @lukasmatena, Enrico Turri @enricoturri1966
///|/ Copyright (c) SuperSlicer 2023 Remi Durand @supermerill
///|/ Copyright (c) Slic3r 2013 - 2016 Alessandro Ranellucci @alranel
///|/ Copyright (c) 2014 Petr Ledvina @ledvinap
///|/
///|/ PrusaSlicer is released under the terms of the AGPLv3 or higher
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/
#include "ExtrusionEntity.hpp"

#include <cmath>
#include <iterator>
#include <limits>
#include <type_traits>

#include "Api/internal/ExtrusionPropertyAccess.hpp"
#include "ClipperUtils.hpp"
#include "Exception.hpp"
#include "ExPolygon.hpp"
#include "Extruder.hpp"
#include "ExtrusionEntityCollection.hpp"
#include "ExtrusionEntityVisitors.hpp"
#include "Flow.hpp"

namespace Slic3r {

static_assert(std::is_nothrow_move_assignable<ExtrusionEntity::Content>::value,
              "Ordered extrusion publication requires non-throwing content moves.");
static_assert(std::is_nothrow_move_assignable<ExtrusionPropertyContainer>::value,
              "Ordered extrusion publication requires non-throwing property moves.");

namespace {

struct EffectiveExtrusionProperty
{
    extrusion_property_type type = extrusion_property_type_invalid;
    const ExtrusionPropertyContainer *owner = nullptr;
};

using EffectiveExtrusionProperties = std::vector<EffectiveExtrusionProperty>;

// Return the currently visible property for a type, after parent properties and
// child overrides have been applied in traversal order.
const EffectiveExtrusionProperty *find_effective_property(const EffectiveExtrusionProperties &properties,
                                                          extrusion_property_type type);

// Add or replace one visible property in the inheritance stack used by the
// simplifier. Extrusion trees usually carry very few properties, so a compact
// vector is simpler and cheaper than a map here.
void set_effective_property(EffectiveExtrusionProperties &properties,
                            extrusion_property_type type,
                            const ExtrusionPropertyContainer &owner);

// Build the property state seen by this entity's children.
EffectiveExtrusionProperties effective_properties_for_children(const EffectiveExtrusionProperties &parent_properties,
                                                               const ExtrusionEntity &entity);

// Check whether a wrapper collection changes the inherited print state. A
// wrapper is transparent only when every explicit property it carries is
// already present on the parent with the same binary payload.
bool explicit_properties_match_parent(const ExtrusionEntity &child,
                                      const EffectiveExtrusionProperties &parent_properties);

// Decide whether a direct child collection may be removed and replaced by its
// children without changing ordering, reversing or inherited property state.
bool is_transparent_collection_child(const ExtrusionEntity &parent,
                                     const ExtrusionEntity &child,
                                     const EffectiveExtrusionProperties &parent_properties);

// Simplify descendants first, then try to remove transparent direct children of
// the current node. The current node itself is never replaced.
bool simplify_extrusion_tree_recursive(ExtrusionEntity &root,
                                       const EffectiveExtrusionProperties &parent_properties);

const EffectiveExtrusionProperty *find_effective_property(const EffectiveExtrusionProperties &properties,
                                                          extrusion_property_type type)
{
    for (const EffectiveExtrusionProperty &property : properties)
        if (property.type == type)
            return &property;
    return nullptr;
}

void set_effective_property(EffectiveExtrusionProperties &properties,
                            extrusion_property_type type,
                            const ExtrusionPropertyContainer &owner)
{
    for (EffectiveExtrusionProperty &property : properties)
        if (property.type == type) {
            property.owner = &owner;
            return;
        }

    properties.push_back(EffectiveExtrusionProperty{ type, &owner });
}

EffectiveExtrusionProperties effective_properties_for_children(const EffectiveExtrusionProperties &parent_properties,
                                                               const ExtrusionEntity &entity)
{
    EffectiveExtrusionProperties out = parent_properties;
    const size_t property_count = ApiInternal::ExtrusionPropertyAccess::property_count(entity);
    for (size_t property_idx = 0; property_idx < property_count; ++property_idx) {
        const extrusion_property_type type =
            ApiInternal::ExtrusionPropertyAccess::property_type_at(entity, property_idx);
        if (type != extrusion_property_type_invalid)
            set_effective_property(out, type, entity);
    }
    return out;
}

bool explicit_properties_match_parent(const ExtrusionEntity &child,
                                      const EffectiveExtrusionProperties &parent_properties)
{
    const size_t property_count = ApiInternal::ExtrusionPropertyAccess::property_count(child);
    for (size_t property_idx = 0; property_idx < property_count; ++property_idx) {
        const extrusion_property_type type =
            ApiInternal::ExtrusionPropertyAccess::property_type_at(child, property_idx);
        const EffectiveExtrusionProperty *parent_property = find_effective_property(parent_properties, type);
        if (parent_property == nullptr || parent_property->owner == nullptr)
            return false;
        if (!ApiInternal::ExtrusionPropertyAccess::same_property_payload(
                child, type, *parent_property->owner))
            return false;
    }
    return true;
}

bool is_transparent_collection_child(const ExtrusionEntity &parent,
                                     const ExtrusionEntity &child,
                                     const EffectiveExtrusionProperties &parent_properties)
{
    if (!child.is_collection())
        return false;
    if (child.can_sort() != parent.can_sort())
        return false;
    if (child.can_reverse() != parent.can_reverse())
        return false;
    return explicit_properties_match_parent(child, parent_properties);
}

bool simplify_extrusion_tree_recursive(ExtrusionEntity &root,
                                       const EffectiveExtrusionProperties &parent_properties)
{
    bool changed = false;
    const EffectiveExtrusionProperties child_properties =
        effective_properties_for_children(parent_properties, root);

    if (root.is_leaf())
        return false;

    ExtrusionEntity::Children &children = root.children();
    for (ExtrusionEntityUPtr &child : children)
        if (child)
            changed = simplify_extrusion_tree_recursive(*child, child_properties) || changed;

    /*
    Splice only after children have been simplified. If a grandchild becomes a
    direct child, the loop checks the same index again so that a wrapper that is
    transparent relative to this parent can also be removed.
    */
    for (size_t child_idx = 0; child_idx < children.size();) {
        ExtrusionEntityUPtr &child = children[child_idx];
        if (!child || !is_transparent_collection_child(root, *child, child_properties)) {
            ++child_idx;
            continue;
        }

        ExtrusionEntityUPtr wrapper = std::move(child);
        ExtrusionEntity::Children grandchildren = std::move(wrapper->children());
        children.erase(children.begin() + child_idx);
        if (!grandchildren.empty()) {
            children.insert(children.begin() + child_idx,
                            std::make_move_iterator(grandchildren.begin()),
                            std::make_move_iterator(grandchildren.end()));
        }
        changed = true;
    }

    return changed;
}

} // namespace

Point ExtrusionNop::NOT_A_POINT = Point((std::numeric_limits<coord_t>::max)(), (std::numeric_limits<coord_t>::max)());

Point ExtrusionEntity::NOT_A_POINT = Point((std::numeric_limits<coord_t>::max)(), (std::numeric_limits<coord_t>::max)());

ExtrusionEntity::Content ExtrusionEntity::clone_content(const Content &content)
{
    if (const ArcPolyline *polyline = std::get_if<ArcPolyline>(&content))
        return *polyline;
    if (const Children *children = std::get_if<Children>(&content)) {
        Children cloned;
        cloned.reserve(children->size());
        for (const ExtrusionEntityUPtr &child : *children) {
            assert(child);
            cloned.emplace_back(child ? ExtrusionEntityUPtr(child->clone()) : nullptr);
        }
        return cloned;
    }
    return std::monostate();
}

bool ExtrusionEntity::has_polyline() const
{
    return std::holds_alternative<ArcPolyline>(m_content);
}

bool ExtrusionEntity::is_leaf() const
{
    return !std::holds_alternative<Children>(m_content);
}

bool ExtrusionEntity::is_nop() const
{
    return std::holds_alternative<std::monostate>(m_content);
}

const ArcPolyline* ExtrusionEntity::polyline_or_null() const
{
    return std::get_if<ArcPolyline>(&m_content);
}

ArcPolyline* ExtrusionEntity::polyline_or_null()
{
    return std::get_if<ArcPolyline>(&m_content);
}

const ArcPolyline& ExtrusionEntity::polyline_ref() const
{
    const ArcPolyline *polyline = this->polyline_or_null();
    assert(polyline != nullptr);
    return *polyline;
}

ArcPolyline& ExtrusionEntity::polyline_ref()
{
    ArcPolyline *polyline = this->polyline_or_null();
    assert(polyline != nullptr);
    return *polyline;
}

void ExtrusionEntity::set_polyline(const ArcPolyline &polyline)
{
    if (Children *children = std::get_if<Children>(&m_content)) {
        assert(false);
        children->insert(children->begin(), std::make_unique<ExtrusionEntity>(m_can_reverse, polyline));
        return;
    }

    m_content = polyline;
    m_can_sort = false;
}

void ExtrusionEntity::set_polyline(ArcPolyline &&polyline)
{
    if (Children *children = std::get_if<Children>(&m_content)) {
        assert(false);
        children->insert(children->begin(), std::make_unique<ExtrusionEntity>(m_can_reverse, std::move(polyline)));
        return;
    }

    m_content = std::move(polyline);
    m_can_sort = false;
}

const ExtrusionEntity::Children& ExtrusionEntity::children() const
{
    static const Children no_children;
    const Children *children = std::get_if<Children>(&m_content);
    return children != nullptr ? *children : no_children;
}

ExtrusionEntity::Children& ExtrusionEntity::children()
{
    return this->ensure_children();
}

size_t ExtrusionEntity::child_count() const
{
    return this->children().size();
}

const ExtrusionEntity& ExtrusionEntity::child(size_t idx) const
{
    assert(!this->is_leaf());
    const Children &children = this->children();
    assert(idx < children.size());
    assert(children[idx]);
    return *children[idx];
}

ExtrusionEntity& ExtrusionEntity::child(size_t idx)
{
    Children &children = this->children();
    assert(idx < children.size());
    assert(children[idx]);
    return *children[idx];
}

ExtrusionEntity::Children& ExtrusionEntity::ensure_children()
{
    if (Children *children = std::get_if<Children>(&m_content))
        return *children;

    Children children;
    if (ArcPolyline *polyline = std::get_if<ArcPolyline>(&m_content)) {
        if (!polyline->empty()) {
            std::unique_ptr<ExtrusionEntity> polyline_child = std::make_unique<ExtrusionEntity>(m_can_reverse, std::move(*polyline));
            polyline_child->m_properties = m_properties;
            children.emplace_back(std::move(polyline_child));
        }
    }

    m_content = std::move(children);
    m_can_sort = true;
    return std::get<Children>(m_content);
}

ExtrusionEntity& ExtrusionEntity::append_child(ExtrusionEntityUPtr &&child)
{
    assert(child);
    Children &children = this->ensure_children();
    children.emplace_back(std::move(child));
    return *children.back();
}

ExtrusionEntity& ExtrusionEntity::append_child(const ExtrusionEntity &child)
{
    return this->append_child(ExtrusionEntityUPtr(child.clone()));
}

ExtrusionEntity& ExtrusionEntity::append_child(ExtrusionEntity &&child)
{
    return this->append_child(ExtrusionEntityUPtr(child.clone_move()));
}

void ExtrusionEntity::insert_child(size_t idx, ExtrusionEntityUPtr &&child)
{
    assert(child);
    Children &children = this->ensure_children();
    if (idx > children.size())
        idx = children.size();
    children.insert(children.begin() + idx, std::move(child));
}

ExtrusionEntity* ExtrusionEntity::emplace_ordered_leaf(
    OrderedLeafPosition position,
    ExistingPropertyPlacement property_placement)
{
    if ((position != OrderedLeafPosition::Before && position != OrderedLeafPosition::After) ||
        (property_placement != ExistingPropertyPlacement::KeepOnParent &&
         property_placement != ExistingPropertyPlacement::MoveWithExistingContent))
        return nullptr;

    std::unique_ptr<ExtrusionEntity> new_leaf = std::make_unique<ExtrusionEntity>(false);
    ExtrusionEntity *new_leaf_ptr = new_leaf.get();

    /*
    A fixed collection already protects direct child order. Keeping properties
    on that parent therefore needs only one vector insertion and preserves the
    existing tree shape.
    */
    Children *current_children = std::get_if<Children>(&m_content);
    const bool is_fixed_collection =
        current_children != nullptr && !m_can_sort && !m_can_reverse;
    if (property_placement == ExistingPropertyPlacement::KeepOnParent && is_fixed_collection) {
        const size_t insertion_idx =
            position == OrderedLeafPosition::Before ? 0 : current_children->size();
        current_children->insert(current_children->begin() + insertion_idx, std::move(new_leaf));
        return new_leaf_ptr;
    }

    /*
    Reserve the complete replacement before touching the live entity. Once
    this preparation succeeds, publishing the wrapper uses only non-throwing
    moves of vectors, unique pointers and property storage.
    */
    std::unique_ptr<ExtrusionEntity> previous_content =
        std::make_unique<ExtrusionEntity>(m_can_reverse);
    ExtrusionEntity *previous_content_ptr = previous_content.get();
    Children replacement;
    replacement.reserve(2);
    if (position == OrderedLeafPosition::Before) {
        replacement.emplace_back(std::move(new_leaf));
        replacement.emplace_back(std::move(previous_content));
    } else {
        replacement.emplace_back(std::move(previous_content));
        replacement.emplace_back(std::move(new_leaf));
    }

    /*
    The wrapper receives the old content and ordering permissions. Moving the
    variant preserves the addresses of existing child objects. The stable
    outer entity is then republished as the fixed two-child sequence.
    */
    previous_content_ptr->m_content = std::move(m_content);
    previous_content_ptr->m_can_sort = m_can_sort;
    previous_content_ptr->m_can_reverse = m_can_reverse;

    if (property_placement == ExistingPropertyPlacement::MoveWithExistingContent) {
        static_cast<ExtrusionPropertyContainer &>(*previous_content_ptr) =
            std::move(static_cast<ExtrusionPropertyContainer &>(*this));
    }

    m_content = std::move(replacement);
    m_can_sort = false;
    m_can_reverse = false;
    return new_leaf_ptr;
}

void ExtrusionEntity::remove_child(size_t idx)
{
    Children &children = this->ensure_children();
    assert(idx < children.size());
    children.erase(children.begin() + idx);
}

void ExtrusionEntity::clear_content()
{
    m_content = std::monostate();
    m_can_sort = false;
}

bool simplify_extrusion_tree(ExtrusionEntity &root)
{
    /*
    The root is kept even if it looks redundant. Callers often hold references
    to that object, while wrapper children are only structural grouping nodes
    created during intermediate processing.
    */
    return simplify_extrusion_tree_recursive(root, EffectiveExtrusionProperties());
}

bool ExtrusionEntity::is_continuous() const
{
    // A sortable node explicitly gives path planners permission to reorder its
    // children. Such a node cannot be treated as a forced continuous sequence,
    // even if the current child order happens to touch end-to-start.
    if (m_can_sort)
        return false;

    if (this->is_leaf())
        return true;

    const ExtrusionEntity *previous_non_empty = nullptr;
    for (const ExtrusionEntityUPtr &child : this->children()) {
        assert(child);
        if (child == nullptr || child->empty())
            continue;
        if (!child->is_continuous())
            return false;
        if (previous_non_empty != nullptr && previous_non_empty->last_point() != child->first_point())
            return false;
        previous_non_empty = child.get();
    }
    return true;
}

bool ExtrusionEntity::is_collection() const
{
    return !this->is_leaf() && !this->is_continuous() && !this->is_loop();
}

bool ExtrusionEntity::is_loop() const
{
    if (this->empty())
        return false;
    return this->is_continuous() && this->first_point() == this->last_point();
}

void ExtrusionEntity::set_can_sort_reverse(bool can_sort, bool can_reverse)
{
    m_can_sort = can_sort;
    m_can_reverse = can_reverse;
}

ExtrusionRole ExtrusionEntity::role() const
{
    if (const ExtrusionAttributes *attributes = this->get_property<ExtrusionAttributes>())
        return attributes->extrusion_role();

    ExtrusionRole out{ ExtrusionRole::None };
    if (!this->is_leaf()) {
        for (const ExtrusionEntityUPtr &child : this->children()) {
            if (!child)
                continue;
            ExtrusionRole child_role = child->role();
            if (out == ExtrusionRole::None)
                out = child_role;
            else if (out != child_role)
                return ExtrusionRole::Mixed;
        }
    }
    return out;
}

bool ExtrusionEntity::has_role(ExtrusionRole test_role) const
{
    if (const ExtrusionAttributes *attributes = this->get_property<ExtrusionAttributes>())
        return (attributes->extrusion_role() & test_role) == test_role;

    if (!this->is_leaf())
        for (const ExtrusionEntityUPtr &child : this->children())
            if (child && child->has_role(test_role))
                return true;
    return false;
}

void ExtrusionEntity::reverse()
{
    if (ArcPolyline *polyline = this->polyline_or_null()) {
        polyline->reverse();
        return;
    }

    Children *children = std::get_if<Children>(&m_content);
    if (children == nullptr)
        return;
    for (ExtrusionEntityUPtr &child : *children)
        if (child && child->can_reverse() && !child->is_loop())
            child->reverse();
    std::reverse(children->begin(), children->end());
}

const Point& ExtrusionEntity::first_point() const
{
    if (const ArcPolyline *polyline = this->polyline_or_null())
        return polyline->empty() ? NOT_A_POINT : polyline->front();
    if (!this->is_leaf())
        for (const ExtrusionEntityUPtr &child : this->children())
            if (child && !child->empty())
                return child->first_point();
    return NOT_A_POINT;
}

const Point& ExtrusionEntity::last_point() const
{
    if (const ArcPolyline *polyline = this->polyline_or_null())
        return polyline->empty() ? NOT_A_POINT : polyline->back();
    if (!this->is_leaf())
        for (Children::const_reverse_iterator it = this->children().rbegin(); it != this->children().rend(); ++it)
            if (*it && !(*it)->empty())
                return (*it)->last_point();
    return NOT_A_POINT;
}

const Point& ExtrusionEntity::middle_point() const
{
    if (const ArcPolyline *polyline = this->polyline_or_null())
        return polyline->empty() ? NOT_A_POINT : polyline->middle();
    if (!this->is_leaf()) {
        const Children &children = this->children();
        if (children.empty())
            return NOT_A_POINT;
        const ExtrusionEntityUPtr &child = children[children.size() / 2];
        if (child && !child->empty())
            return child->middle_point();
    }
    return NOT_A_POINT;
}

void ExtrusionEntity::polygons_covered_by_width(Polygons &out, const float scaled_epsilon) const
{
    if (const ArcPolyline *polyline = this->polyline_or_null()) {
        const ExtrusionAttributes *attributes = this->get_property<ExtrusionAttributes>();
        if (attributes != nullptr)
            out = union_(out, offset(polyline->to_polyline(), scale_d(attributes->width / 2) + scaled_epsilon));
        return;
    }

    if (!this->is_leaf())
        for (const ExtrusionEntityUPtr &child : this->children())
            if (child)
                child->polygons_covered_by_width(out, scaled_epsilon);
}

void ExtrusionEntity::polygons_covered_by_spacing(Polygons &out, const float spacing_ratio, const float scaled_epsilon) const
{
    if (const ArcPolyline *polyline = this->polyline_or_null()) {
        const ExtrusionAttributes *attributes = this->get_property<ExtrusionAttributes>();
        if (attributes == nullptr)
            return;
        const bool bridge = attributes->extrusion_role().is_bridge() || (attributes->width * 4 < attributes->height);
        Flow flow = bridge ? Flow::bridging_flow(attributes->width, 0.f) :
                             Flow::new_from_width(attributes->width, 0.f, attributes->height, spacing_ratio);
        if (out.empty()) {
            out = offset(polyline->to_polyline(), 0.5f * float(flow.scaled_spacing()) + scaled_epsilon,
                         Slic3r::ClipperLib::jtMiter, 10);
        } else {
            out = union_(out,
                         offset(polyline->to_polyline(), 0.5f * float(flow.scaled_spacing()) + scaled_epsilon,
                                Slic3r::ClipperLib::jtMiter, 10));
        }
        return;
    }

    if (!this->is_leaf())
        for (const ExtrusionEntityUPtr &child : this->children())
            if (child)
                child->polygons_covered_by_spacing(out, spacing_ratio, scaled_epsilon);
}

ArcPolyline ExtrusionEntity::as_polyline() const
{
    if (const ArcPolyline *polyline = this->polyline_or_null())
        return *polyline;

    ArcPolyline out;
    if (!this->is_leaf())
        for (const ExtrusionEntityUPtr &child : this->children())
            if (child)
                out.append(child->as_polyline());
    return out;
}

void ExtrusionEntity::collect_polylines(ArcPolylines &dst) const
{
    if (const ArcPolyline *polyline = this->polyline_or_null()) {
        if (!polyline->empty())
            dst.emplace_back(*polyline);
        return;
    }

    if (!this->is_leaf())
        for (const ExtrusionEntityUPtr &child : this->children())
            if (child)
                child->collect_polylines(dst);
}

void ExtrusionEntity::collect_points(Points &dst) const
{
    if (const ArcPolyline *polyline = this->polyline_or_null()) {
        append(dst, polyline->to_polyline().points);
        return;
    }

    if (!this->is_leaf())
        for (const ExtrusionEntityUPtr &child : this->children())
            if (child)
                child->collect_points(dst);
}

void ExtrusionEntity::visit(ExtrusionVisitor &visitor)
{
    visitor.use(*this);
}

void ExtrusionEntity::visit(ExtrusionVisitorConst &visitor) const
{
    visitor.use(*this);
}

void ExtrusionEntity::visit(ExtrusionVisitor &&visitor)
{
    this->visit(visitor);
}

void ExtrusionEntity::visit(ExtrusionVisitorConst &&visitor) const
{
    this->visit(visitor);
}

void ExtrusionNop::visit(ExtrusionVisitor &visitor)
{
    visitor.use(*this);
}

void ExtrusionNop::visit(ExtrusionVisitorConst &visitor) const
{
    visitor.use(*this);
}

void ExtrusionPath::visit(ExtrusionVisitor &visitor)
{
    // A split path is structurally a collection even though its stable object
    // still has the ExtrusionPath dynamic type. Dispatch it as the generic
    // entity so recursive visitors descend into the published fragments.
    if (this->has_polyline())
        visitor.use(*this);
    else
        ExtrusionEntity::visit(visitor);
}

void ExtrusionPath::visit(ExtrusionVisitorConst &visitor) const
{
    if (this->has_polyline())
        visitor.use(*this);
    else
        ExtrusionEntity::visit(visitor);
}

void ExtrusionMultiPath::visit(ExtrusionVisitor &visitor)
{
    visitor.use(*this);
}

void ExtrusionMultiPath::visit(ExtrusionVisitorConst &visitor) const
{
    visitor.use(*this);
}

void ExtrusionLoop::visit(ExtrusionVisitor &visitor)
{
    visitor.use(*this);
}

void ExtrusionLoop::visit(ExtrusionVisitorConst &visitor) const
{
    visitor.use(*this);
}

coordf_t ExtrusionEntity::length() const
{
    if (const ArcPolyline *polyline = this->polyline_or_null())
        return polyline->length();

    coordf_t len = 0;
    if (!this->is_leaf())
        for (const ExtrusionEntityUPtr &child : this->children())
            if (child)
                len += child->length();
    return len;
}

bool ExtrusionEntity::empty() const
{
    if (const ArcPolyline *polyline = this->polyline_or_null())
        return polyline->empty();

    if (!this->is_leaf())
        for (const ExtrusionEntityUPtr &child : this->children())
            if (child && !child->empty())
                return false;
    return true;
}

double ExtrusionEntity::total_volume() const
{
    if (this->has_polyline()) {
        const ExtrusionAttributes *attributes = this->get_property<ExtrusionAttributes>();
        return attributes != nullptr ? attributes->mm3_per_mm * unscaled(this->length()) : 0.;
    }

    double volume = 0.;
    if (!this->is_leaf())
        for (const ExtrusionEntityUPtr &child : this->children())
            if (child)
                volume += child->total_volume();
    return volume;
}

ExtrusionPropertyOverhang &ExtrusionPath::overhang_attributes_mutable() {
    return this->get_or_add_property<ExtrusionPropertyOverhang>();
}

const ExtrusionPropertyOverhang *ExtrusionPath::overhang_attributes() const {
    return this->get_property<ExtrusionPropertyOverhang>();
}

void ExtrusionPath::intersect_expolygons(const ExPolygons &collection, ExtrusionEntityCollection *retval) const
{
    this->_inflate_collection(intersection_pl(Polylines{this->polyline().to_polyline()}, collection), retval);
}

void ExtrusionPath::subtract_expolygons(const ExPolygons &collection, ExtrusionEntityCollection *retval) const
{
    this->_inflate_collection(diff_pl(Polylines{this->polyline().to_polyline()}, collection), retval);
}

void ExtrusionPath::clip_end(coordf_t distance) { this->polyline().clip_end(distance); }

coordf_t ExtrusionPath::length() const
{
    return this->has_polyline() ? this->polyline().length() : ExtrusionEntity::length();
}

void ExtrusionPath::_inflate_collection(const Polylines &polylines, ExtrusionEntityCollection *collection) const
{
    ExtrusionEntitiesPtr to_add;
    for (const Polyline &polyline : polylines)
        to_add.push_back(new ExtrusionPath(ArcPolyline{polyline}, this->attributes(), this->clone_properties(), this->can_reverse()));
    collection->append(std::move(to_add));
}

void ExtrusionPath::polygons_covered_by_width(Polygons &out, const float scaled_epsilon) const
{
    if (!this->has_polyline()) {
        ExtrusionEntity::polygons_covered_by_width(out, scaled_epsilon);
        return;
    }

    //polygons_append(out, offset(this->polyline().to_polyline(), double(scale_(attributes().width / 2)) + scaled_epsilon));
    out = union_(out, offset(this->polyline().to_polyline(), scale_d(attributes().width / 2) + scaled_epsilon));
}

void ExtrusionPath::polygons_covered_by_spacing(Polygons &out, const float spacing_ratio, const float scaled_epsilon) const
{
    if (!this->has_polyline()) {
        ExtrusionEntity::polygons_covered_by_spacing(out, spacing_ratio, scaled_epsilon);
        return;
    }

    // Instantiating the Flow class to get the line spacing.
    // Don't know the nozzle diameter, setting to zero. It shall not matter it shall be optimized out by the compiler.
    bool bridge = this->role().is_bridge() || (this->width() * 4 < this->height());
    assert(!bridge || attributes().width == attributes().height);
    // TODO: check BRIDGE_FLOW here
    Flow flow = bridge ? Flow::bridging_flow(attributes().width, 0.f) :
                         Flow::new_from_width(attributes().width, 0.f, attributes().height, spacing_ratio);
    if (out.empty()) {
        out = offset(this->polyline().to_polyline(), 0.5f * float(flow.scaled_spacing()) + scaled_epsilon,
                     Slic3r::ClipperLib::jtMiter, 10);
    } else {
        out = union_(out,
                     offset(this->polyline().to_polyline(), 0.5f * float(flow.scaled_spacing()) + scaled_epsilon,
                            Slic3r::ClipperLib::jtMiter, 10));
    }
}

//note: don't suppport arc
double ExtrusionLoop::area() const
{
    double a = 0;
    for (const ExtrusionPath &path : this->paths()) {
        assert(path.size() >= 2);
        if (path.size() >= 2) {
            if (path.polyline().has_arc()) {
                Polyline poly = path.polyline().to_polyline();
                Point prev = poly.front();
                for (size_t idx = 1; idx < poly.size(); ++idx) {
                    const Point &curr = poly[idx];
                    a += cross2(prev.cast<double>(), curr.cast<double>());
                    prev = curr;
                }
            } else {
                // Assumming that the last point of one path segment is repeated at the start of the following path segment.
                Point prev = path.polyline().front();
                for (size_t idx = 1; idx < path.polyline().size(); ++idx) {
                    const Point &curr = path.polyline().get_point(idx);
                    a += cross2(prev.cast<double>(), curr.cast<double>());
                    prev = curr;
                }
            }
        }
    }
    return a * 0.5;
}

bool ExtrusionLoop::is_counter_clockwise() const {
    return this->area() > 0;
}

bool ExtrusionLoop::is_clockwise() const { return !is_counter_clockwise(); }

void ExtrusionLoop::reverse()
{
    for (ExtrusionPath &path : this->paths())
        path.reverse();
    std::reverse(this->paths().begin(), this->paths().end());
}

Polygon ExtrusionLoop::polygon() const
{
    Polygon polygon;
    for (const ExtrusionPath &path : this->paths()) {
        // for each polyline, append all points except the last one (because it coincides with the first one of the next polyline)
        Polyline poly = path.polyline().to_polyline();
        polygon.points.insert(polygon.points.end(), poly.begin(), poly.end() - 1);
    }
    return polygon;
}

Polygon polygon(const ExtrusionEntity &entity)
{
    assert(entity.is_loop());

    struct PolygonVisitor : ExtrusionTreeConstVisitor<false> {
        Polygon polygon;

        void visit_leaf(const ExtrusionEntity &leaf) override
        {
            const ArcPolyline *arc_polyline = leaf.polyline_or_null();
            if (arc_polyline == nullptr || arc_polyline->empty())
                return;

            // The tree represents one continuous loop. Adjacent leaf polylines
            // share an endpoint, so skip the first point of a leaf when it is
            // already the last point copied from the previous leaf.
            Polyline polyline = arc_polyline->to_polyline();
            size_t first_point_idx = 0;
            if (!polygon.points.empty() && !polyline.points.empty() && polygon.points.back() == polyline.points.front())
                first_point_idx = 1;
            if (first_point_idx < polyline.points.size())
                polygon.points.insert(polygon.points.end(), polyline.points.begin() + first_point_idx, polyline.points.end());
        }
    } visitor;

    visitor.traverse(entity);
    if (!visitor.polygon.points.empty() && visitor.polygon.points.front() == visitor.polygon.points.back())
        visitor.polygon.points.pop_back();
    return visitor.polygon;
}

ArcPolyline ExtrusionLoop::as_polyline() const
{
    ArcPolyline polyline;
    for (const ExtrusionPath &path : this->paths()) {
        polyline.append(path.as_polyline());
    }
    return polyline;
}

double ExtrusionLoop::length() const
{
    double len = 0;
    for (const ExtrusionPath &path : this->paths())
        len += path.polyline().length();
    return len;
}

ExtrusionRole ExtrusionLoop::role() const
{
    if (this->paths().empty())
        return ExtrusionRole::None;
    ExtrusionRole role = this->paths().front().role();
    for (const ExtrusionPath &path : this->paths())
        if (role != path.role()) {
            // ignore travel role
            if (role == ExtrusionRole::Travel) {
                role = path.role();
            } else if (path.role() != ExtrusionRole::Travel) {
                return ExtrusionRole::Mixed;
            }
        }
    return role;
}
bool ExtrusionLoop::has_role(ExtrusionRole test_role) const
{
    if (this->paths().empty())
        return false;
    for (const ExtrusionPath &path : this->paths())
        if (path.has_role(test_role)) {
            return true;
        }
    return false;
}

bool ExtrusionLoop::split_at_vertex(const Point &point, const double scaled_epsilon)
{
    ExtrusionPathView paths = this->paths();
    for (ExtrusionPathView::iterator path = paths.begin(); path != paths.end(); ++path) {
        if (int idx = path->polyline().find_point(point, scaled_epsilon); idx != -1) {
            if (paths.size() == 1) {
                if (idx == 0 || idx == path->size() - 1) {
                    assert(this->first_point().distance_to(point) <= scaled_epsilon);
                    return true;
                }
                // just change the order of points
                ArcPolyline p1, p2;
                path->polyline().split_at_index(idx, p1, p2);
                if (p1.is_valid() && p2.is_valid()) {
                    p2.append(std::move(p1));
                    path->polyline().swap(p2); // swap points & fitting result
                }
            } else if (idx > 0) {
                if (idx < path->size() - 1) {
                    // new paths list starts with the second half of current path
                    ExtrusionPaths new_paths;
                    ArcPolyline p1, p2;
                    path->polyline().split_at_index(idx, p1, p2);
                    new_paths.reserve(paths.size() + 1);
                    {
                        ExtrusionPath p = *path;
                        p.polyline().swap(p2);
                        if (p.polyline().is_valid())
                            new_paths.push_back(p);
                    }

                    // then we add all paths until the end of current path list
                    new_paths.insert(new_paths.end(), path + 1, paths.end()); // not including this path

                    // then we add all paths since the beginning of current list up to the previous one
                    new_paths.insert(new_paths.end(), paths.begin(), path); // not including this path

                    // finally we add the first half of current path
                    {
                        ExtrusionPath p = *path;
                        p.polyline().swap(p1);
                        if (p.polyline().is_valid())
                            new_paths.push_back(p);
                    }
                    // we can now override the old path list with the new one and stop looping
                    this->paths() = std::move(new_paths);
                } else {
                    // last point
                    assert((path)->last_point().distance_to(point) <= scaled_epsilon);
                    assert((path + 1)->first_point().distance_to(point) <= scaled_epsilon);
                    ExtrusionPaths new_paths;
                    new_paths.reserve(paths.size());
                    // then we add all paths until the end of current path list
                    new_paths.insert(new_paths.end(), path + 1, paths.end()); // not including this path
                    // then we add all paths since the beginning of current list up to the previous one
                    new_paths.insert(new_paths.end(), paths.begin(), path + 1); // including this path
                    // we can now override the old path list with the new one and stop looping
                    this->paths() = std::move(new_paths);
                }
            } else {
                // else first point ->
                // if first path - nothign to change.
                // else, then impossible as it's also the last point of the previous path.
                assert(path == paths.begin());
                assert(path->first_point().distance_to(point) <= scaled_epsilon);
            }
            assert(this->first_point().distance_to(point) <= scaled_epsilon);
            return true;
        }
    }
    // The point was not found.
    return false;
}

ExtrusionLoop::ClosestPathPoint ExtrusionLoop::get_closest_path_and_point(const Point &point, bool prefer_non_overhang) const
{
    // Find the closest path and closest point belonging to that path. Avoid overhangs, if asked for.
    ClosestPathPoint out{0, 0};
    double           min2 = std::numeric_limits<double>::max();
    ClosestPathPoint best_non_overhang{0, 0};
    double           min2_non_overhang = std::numeric_limits<double>::max();
    size_t path_idx = 0;
    for (const ExtrusionPath &path : this->paths()) {
        std::pair<int, Point> foot_pt_ = path.polyline().foot_pt(point);
        double                d2       = (foot_pt_.second - point).cast<double>().squaredNorm();
        if (d2 < min2) {
            out.foot_pt     = foot_pt_.second;
            out.path_idx    = path_idx;
            out.segment_idx = foot_pt_.first;
            min2            = d2;
        }
        if (prefer_non_overhang && !path.role().is_bridge() && d2 < min2_non_overhang) {
            best_non_overhang.foot_pt     = foot_pt_.second;
            best_non_overhang.path_idx    = path_idx;
            best_non_overhang.segment_idx = foot_pt_.first;
            min2_non_overhang             = d2;
        }
        ++path_idx;
    }
    if (prefer_non_overhang && min2_non_overhang != std::numeric_limits<double>::max()) {
        // Only apply the non-overhang point if there is one.
        out = best_non_overhang;
    }
    return out;
}

// Splitting an extrusion loop, possibly made of multiple segments, some of the segments may be bridging.
void ExtrusionLoop::split_at(const Point &point, bool prefer_non_overhang, const double scaled_epsilon)
{
    if (this->paths().empty())
        return;
    ExtrusionLoop::ClosestPathPoint close_p = get_closest_path_and_point(point, prefer_non_overhang);
    // Snap p to start or end of segment_idx if closer than scaled_epsilon.
    //{
        const Point pt1 = this->paths()[close_p.path_idx].polyline().get_point(close_p.segment_idx);
        const Point  pt2   = this->paths()[close_p.path_idx].polyline().get_point(close_p.segment_idx + 1);
        // Use close_p.foot_pt instead of point for the comparison, as it's the one that will be used.
        double       d2_1 = (close_p.foot_pt - pt1).cast<double>().squaredNorm();
        double       d2_2 = (close_p.foot_pt - pt2).cast<double>().squaredNorm();
        const double thr2 = scaled_epsilon * scaled_epsilon;
        if (d2_1 < d2_2) {
            if (d2_1 < thr2)
                close_p.foot_pt = pt1;
        } else {
            if (d2_2 < thr2)
                close_p.foot_pt = pt2;
        }
    //}

    // now split path_idx in two parts
    const ExtrusionPath &path = this->paths()[close_p.path_idx];
    assert(path.polyline().is_valid());
    ExtrusionPath        p1(path.attributes(), path.clone_properties(), can_reverse());
    ExtrusionPath        p2(path.attributes(), path.clone_properties(), can_reverse());
    path.polyline().split_at(close_p.foot_pt, p1.polyline(), p2.polyline());

    if (this->paths().size() == 1) {
        if (p1.polyline().size() < 2) {
            this->paths().front().polyline() = std::move(p2.polyline());
        } else if (p2.polyline().size() < 2) {
            this->paths().front().polyline() = std::move(p1.polyline());
        } else {
            p2.polyline().append(std::move(p1.polyline()));
            this->paths().front().polyline() = std::move(p2.polyline());
        }
    } else {
        // install the begining of the new paths
        if (p2.polyline().size() >= 2) {
            this->paths()[close_p.path_idx].polyline() = std::move(p2.polyline());
        } else {
            this->paths().erase(this->paths().begin() + close_p.path_idx);
        }
        //rotate
        if (close_p.path_idx > 0) {
            std::rotate(this->paths().begin(), this->paths().begin() + close_p.path_idx, this->paths().end());
        }
        // install the end
        if (p1.polyline().size() >= 2) {
            this->paths().push_back(std::move(p1));
        }
    }
    // check if it's doing its job.
#ifdef _DEBUG
    Point last_pt = this->last_point();
    for (const ExtrusionPath &path : paths()) {
        assert(last_pt == path.first_point());
        for (int i = 1; i < path.polyline().size(); ++i)
            assert(!path.polyline().get_point(i - 1).coincides_with_epsilon(path.polyline().get_point(i)));
        last_pt = path.last_point();
    }
    assert(close_p.foot_pt.coincides_with_epsilon(this->first_point()));
    //assert(point.distance_to(this->first_point()) <= scaled_epsilon); // can be false, still ok?
#endif
}

ExtrusionPaths clip_end(ExtrusionPaths &paths, coordf_t distance)
{
    ExtrusionPaths removed;

    while (distance > 0 && !paths.empty()) {
        ExtrusionPath &last = paths.back();
        removed.push_back(last);
        coordf_t len = last.length();
        if (len <= distance) {
            paths.pop_back();
            distance -= len;
        } else {
            last.polyline().clip_end(distance);
            removed.back().polyline().clip_start(removed.back().polyline().length() - distance);
            break;
        }
    }
    for(auto& path : paths)
        DEBUG_TREE_VISIT(path, LoopAssertVisitor())
    std::reverse(removed.begin(), removed.end());
    return removed;
}

//bool ExtrusionLoop::has_overhang_point(const Point &point) const
//{
//    for (const ExtrusionPath &path : this->paths()) {
//        int pos = path.polyline().find_point(point);
//        if (pos != -1) {
//            // point belongs to this path
//            // we consider it overhang only if it's not an endpoint
//            return (path.role().is_bridge() && pos > 0 && pos != int(path.polyline().size()) - 1);
//        }
//    }
//    return false;
//}

void ExtrusionLoop::polygons_covered_by_width(Polygons &out, const float scaled_epsilon) const
{
    for (const ExtrusionPath &path : this->paths())
        path.polygons_covered_by_width(out, scaled_epsilon);
}

void ExtrusionLoop::polygons_covered_by_spacing(Polygons &out, const float spacing_ratio, const float scaled_epsilon) const
{
    for (const ExtrusionPath &path : this->paths())
        path.polygons_covered_by_spacing(out, spacing_ratio, scaled_epsilon);
}

//TODO del
//double ExtrusionLoop::min_mm3_per_mm() const
//{
//    double min_mm3_per_mm = std::numeric_limits<double>::max();
//    for (const ExtrusionPath &path : this->paths())
//        min_mm3_per_mm = std::min(min_mm3_per_mm, path.min_mm3_per_mm());
//    return min_mm3_per_mm;
//}

} // namespace Slic3r
