///|/ Copyright (c) SuperSlicer 2026 Durand Rémi @supermerill
///|/ Copyright (c) Prusa Research 2016 - 2023 Vojtěch Bubník @bubnikv, Lukáš Matěna @lukasmatena, Enrico Turri @enricoturri1966
///|/ Copyright (c) Slic3r 2013 - 2016 Alessandro Ranellucci @alranel
///|/ Copyright (c) 2014 Petr Ledvina @ledvinap
///|/
///|/ PrusaSlicer is released under the terms of the AGPLv3 or higher
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/
#include "ExtrusionEntityVisitors.hpp"

#include <cmath>
#include <cstdint>
#include <iostream>

#include "ConfigOption.hpp"

namespace Slic3r {

void ExtrusionVisitorRecursive::default_use(ExtrusionEntity &entity)
{
    if (entity.is_leaf())
        return;

    for (ExtrusionEntityUPtr &child : entity.children())
        if (child)
            child->visit(*this);
}

void ExtrusionVisitorRecursiveConst::default_use(const ExtrusionEntity &entity)
{
    if (entity.is_leaf())
        return;

    for (const ExtrusionEntityUPtr &child : entity.children())
        if (child)
            child->visit(*this);
}

void ExtrusionPrinter::begin_entity()
{
    if (!m_first_child_stack.empty()) {
        if (!m_first_child_stack.back())
            ss << ",";
        m_first_child_stack.back() = false;
    }
}

void ExtrusionPrinter::begin_property(bool &first_property, const char *name)
{
    if (!first_property)
        ss << ",";
    first_property = false;
    ss << (json ? "\"" : "") << name << (json ? "\":{" : "={");
}

void ExtrusionPrinter::begin_property_field(bool &first_field, const char *name)
{
    if (!first_field)
        ss << ",";
    first_field = false;
    ss << (json ? "\"" : "") << name << (json ? "\":" : "=");
}

void ExtrusionPrinter::print_bool_value(bool value)
{
    ss << (value ? "true" : "false");
}

void ExtrusionPrinter::print_string_value(const std::string &value)
{
    if (!json) {
        ss << value;
        return;
    }

    ss << "\"";
    for (const char c : value) {
        if (c == '\\' || c == '"')
            ss << "\\" << c;
        else if (c == '\n')
            ss << "\\n";
        else if (c == '\r')
            ss << "\\r";
        else if (c == '\t')
            ss << "\\t";
        else
            ss << c;
    }
    ss << "\"";
}

bool ExtrusionPrinter::print_properties(const ExtrusionEntity &entity, const char *prefix, const char *suffix)
{
    const bool has_known_property =
        entity.get_property<ExtrusionAttributes>() != nullptr ||
        entity.get_property<ExtrusionPropertySpeed>() != nullptr ||
        entity.get_property<ExtrusionPropertyModifier>() != nullptr ||
        entity.get_property<ExtrusionPropertyCustomGcode>() != nullptr ||
        entity.get_property<ExtrusionPropertySpecialCommand>() != nullptr ||
        entity.get_property<ExtrusionPropertyOverhang>() != nullptr ||
        entity.get_property<ExtrusionPropertyZOffset>() != nullptr ||
        entity.get_property<ExtrusionPropertyLoopRole>() != nullptr ||
        entity.get_property<ExtrusionPropertyInfill>() != nullptr;
    if (!has_known_property)
        return false;

    // The debug printer can only decode built-in property payloads. Plugin
    // properties may use arbitrary byte layouts, so they stay hidden here unless
    // a dedicated decoder is added for their type.
    //ss << prefix << (json ? "\"properties\":{" : "properties={");
    bool first_property = false;

    if (const ExtrusionAttributes *property = entity.get_property<ExtrusionAttributes>()) {
        bool first_field = true;
        this->begin_property(first_property, "attributes");
        this->begin_property_field(first_field, "role");
        this->print_string_value(role_to_code(property->extrusion_role()));
        this->begin_property_field(first_field, "mm3_per_mm");
        ss << property->mm3_per_mm;
        this->begin_property_field(first_field, "width");
        ss << property->width;
        this->begin_property_field(first_field, "height");
        ss << property->height;
        this->begin_property_field(first_field, "no_seam");
        this->print_bool_value(property->no_seam != 0);
        ss << "}";
    }

    if (const ExtrusionPropertySpeed *property = entity.get_property<ExtrusionPropertySpeed>()) {
        bool first_field = true;
        this->begin_property(first_property, "speed");
        this->begin_property_field(first_field, "speed_mm_per_s");
        ss << property->speed_mm_per_s;
        this->begin_property_field(first_field, "accel_mm_per_s2");
        ss << property->accel_mm_per_s2;
        this->begin_property_field(first_field, "pressure_adv");
        ss << property->pressure_adv;
        this->begin_property_field(first_field, "fan_speed_percent");
        ss << property->fan_speed_percent;
        this->begin_property_field(first_field, "temperature_C");
        ss << property->temperature_C;
        ss << "}";
    }

    if (const ExtrusionPropertyModifier *property = entity.get_property<ExtrusionPropertyModifier>()) {
        bool first_field = true;
        this->begin_property(first_property, "modifier");
        this->begin_property_field(first_field, "enforce_travel");
        this->print_bool_value(property->enforce_travel != 0);
        this->begin_property_field(first_field, "enforce_retraction");
        this->print_bool_value(property->enforce_retraction != 0);
        this->begin_property_field(first_field, "enforce_unlift");
        this->print_bool_value(property->enforce_unlift != 0);
        this->begin_property_field(first_field, "disable_retraction");
        this->print_bool_value(property->disable_retraction != 0);
        this->begin_property_field(first_field, "disable_lift");
        this->print_bool_value(property->disable_lift != 0);
        this->begin_property_field(first_field, "toolchange_retraction");
        this->print_bool_value(property->toolchange_retraction != 0);
        ss << "}";
    }

    if (const ExtrusionPropertyCustomGcode *property = entity.get_property<ExtrusionPropertyCustomGcode>()) {
        bool first_field = true;
        this->begin_property(first_property, "custom_gcode");
        this->begin_property_field(first_field, "kind");
        this->print_string_value(property->kind == C_EXTRUSION_CUSTOM_GCODE_COMMENT ? "comment" : "gcode");
        this->begin_property_field(first_field, "text_id");
        ss << property->text_id;
        const std::string text = entity.custom_gcode_string(*property);
        if (!text.empty()) {
            this->begin_property_field(first_field, "text");
            this->print_string_value(text);
        }
        ss << "}";
    }

    if (const ExtrusionPropertySpecialCommand *property = entity.get_property<ExtrusionPropertySpecialCommand>()) {
        const char *command_name = "unknown";
        switch (property->command_code()) {
        case ExtrusionPropertySpecialCommand::Code::TOOLCHANGE: command_name = "toolchange"; break;
        case ExtrusionPropertySpecialCommand::Code::SAVE_AND_RESET_SPEED_RATIO: command_name = "save_and_reset_speed_ratio"; break;
        case ExtrusionPropertySpecialCommand::Code::RESTORE_SPEED_RATIO: command_name = "restore_speed_ratio"; break;
        case ExtrusionPropertySpecialCommand::Code::FLUSH_PLANNER_QUEUE: command_name = "flush_planner_queue"; break;
        case ExtrusionPropertySpecialCommand::Code::EXTRUSION: command_name = "extrusion"; break;
        case ExtrusionPropertySpecialCommand::Code::RETRACT: command_name = "retract"; break;
        case ExtrusionPropertySpecialCommand::Code::PAUSE: command_name = "pause"; break;
        case ExtrusionPropertySpecialCommand::Code::WAIT_FOR_TEMP: command_name = "wait_for_temp"; break;
        case ExtrusionPropertySpecialCommand::Code::DISABLE_PREVIEW: command_name = "disable_preview"; break;
        case ExtrusionPropertySpecialCommand::Code::ENABLE_PREVIEW: command_name = "enable_preview"; break;
        case ExtrusionPropertySpecialCommand::Code::EXTRUDER_CURRENT: command_name = "extruder_current"; break;
        }

        bool first_field = true;
        this->begin_property(first_property, "special_command");
        this->begin_property_field(first_field, "code");
        this->print_string_value(command_name);
        this->begin_property_field(first_field, "extra_data");
        ss << property->extra_data;
        ss << "}";
    }

    if (const ExtrusionPropertyOverhang *property = entity.get_property<ExtrusionPropertyOverhang>()) {
        bool first_field = true;
        this->begin_property(first_property, "overhang");
        this->begin_property_field(first_field, "start_distance_from_prev_layer");
        ss << property->start_distance_from_prev_layer;
        this->begin_property_field(first_field, "end_distance_from_prev_layer");
        ss << property->end_distance_from_prev_layer;
        this->begin_property_field(first_field, "proximity_to_curled_lines");
        ss << property->proximity_to_curled_lines;
        this->begin_property_field(first_field, "has_full_overhangs_flow");
        this->print_bool_value(property->has_full_overhangs_flow != 0);
        this->begin_property_field(first_field, "has_full_overhangs_speed");
        this->print_bool_value(property->has_full_overhangs_speed != 0);
        this->begin_property_field(first_field, "has_dynamic_overhangs_flow");
        this->print_bool_value(property->has_dynamic_overhangs_flow != 0);
        this->begin_property_field(first_field, "has_dynamic_overhangs_speed");
        this->print_bool_value(property->has_dynamic_overhangs_speed != 0);
        ss << "}";
    }

    if (const ExtrusionPropertyZOffset *property = entity.get_property<ExtrusionPropertyZOffset>()) {
        bool first_field = true;
        this->begin_property(first_property, "z_offset");
        this->begin_property_field(first_field, "z_offset");
        ss << property->z_offset;
        ss << "}";
    }

    if (const ExtrusionPropertyLoopRole *property = entity.get_property<ExtrusionPropertyLoopRole>()) {
        bool first_field = true;
        this->begin_property(first_property, "perimeter");
        this->begin_property_field(first_field, "perimeter_idx");
        ss << property->perimeter_idx;
        this->begin_property_field(first_field, "flags");
        ss << property->perimeter_flags;
        this->begin_property_field(first_field, "flags_text");
        this->print_string_value(looprole_to_code(ExtrusionLoopRole(property->perimeter_flags)));
        ss << "}";
    }

    if (const ExtrusionPropertyInfill *property = entity.get_property<ExtrusionPropertyInfill>()) {
        bool first_field = true;
        this->begin_property(first_property, "infill");
        this->begin_property_field(first_field, "source_surface_id");
        ss << property->source_surface_id;
        ss << "}";
    }

    ss << suffix;
    return true;
}

void ExtrusionPrinter::print_equals() {
    ss << (json ? ":" : "=");
}

void ExtrusionPrinter::print_leaf(const ExtrusionEntity &entity)
{
    const ArcPolyline *polyline = entity.polyline_or_null();
    if (polyline == nullptr)
        return;

    this->begin_entity();
    const bool has_z_profile = polyline->has_z_offset();
    if (has_z_profile) {
        ss << ",";
        print_string_value("is_3D");
        print_equals();
        print_bool_value(true);
    }
    if (json) {
        ss << ",";
        print_string_value("points");
        print_equals();
        ss << "[";
    } else {
        ss << "[";
    }
    for (int i = 0; i < polyline->size(); i++) {
        if (i != 0)
            ss << ",";
        double x = (mult * (polyline->get_point(i).x()));
        double y = (mult * (polyline->get_point(i).y()));
        if (has_z_profile) {
            double z = mult * polyline->z_offset(size_t(i));
            ss << std::fixed << "[" << (trunc>0?(int(x*trunc))/double(trunc):x) << "," << (trunc>0?(int(y*trunc))/double(trunc):y) << "," << (trunc>0?(int(z*trunc))/double(trunc):z) << "]";
        } else {
            ss << std::fixed << "["<<(trunc>0?(int(x*trunc))/double(trunc):x) << "," << (trunc>0?(int(y*trunc))/double(trunc):y) <<"]";
        }
    }
    ss << "]";
}

void ExtrusionPrinter::enter_node(const ExtrusionEntity &entity)
{
    if (!m_first_child_stack.empty()) {
        if (m_first_child_stack.back()) {
            m_first_child_stack.back() = false;
        } else {
            ss << ",";
        }
    }
    ss << "{";
    print_string_value("type");
    print_equals();
    //this->begin_entity();
    if (entity.child_count() > 0 && entity.is_loop()) {
        print_string_value("loop");
    } else if (entity.child_count() > 0 && entity.is_continuous()) {
        print_string_value("multipath");
    } else if (entity.child_count() > 0) {
        print_string_value("collection");
    } else if (entity.is_nop()) {
        print_string_value("nop");
    } else if (entity.is_leaf()) {
        print_string_value("path");
    } else {
        print_string_value("error");
    }
    if (!entity.can_sort()) {
        ss << ",";
        print_string_value("no_sort");
        print_equals();
        print_bool_value(true);
    }
    if (!entity.can_reverse()) {
        ss << ",";
        print_string_value("oriented");
        print_equals();
        print_bool_value(true);
    }
    if (json) {
        this->print_properties(entity, "", "");
    } else {
        this->print_properties(entity, " ");
    }
    if (entity.child_count() > 0) {
        ss << ",";
        print_string_value("childs");
        print_equals();
        ss << "[";
    }
    m_first_child_stack.push_back(true);
}

void ExtrusionPrinter::visit_leaf(const ExtrusionEntity &entity)
{
    this->print_leaf(entity);
}

void ExtrusionPrinter::leave_node(const ExtrusionEntity& entity)
{
    if (entity.child_count() > 0) {
        ss << "]";
    }
    ss << "}";
    assert(!m_first_child_stack.empty());
    m_first_child_stack.pop_back();
}

#ifdef _DEBUG
const char *debug_print(const ExtrusionEntity *entity)
{
    static std::string out;
    if (entity != nullptr) {
        ExtrusionPrinter printer(0.000001, 0, true);
        printer.traverse(*entity);
        out = "";
        out += printer.str();
        out+="";
        return out.c_str();
    } else {
        return "";
    }
}
#endif

void ExtrusionLength::visit_leaf(const ExtrusionEntity &entity)
{
    dist += entity.length();
}

double ExtrusionVolume::get(const ExtrusionEntityCollection &coll) {
    this->traverse(coll);
    return volume;
}

void ExtrusionModifyFlow::set(ExtrusionEntityCollection &coll) {
    this->traverse(coll);
}

void HasRoleVisitor::visit_leaf(const ExtrusionEntity& entity)
{
    if (found)
        return;
    const ExtrusionAttributes *attributes = entity.get_property<ExtrusionAttributes>();
    found = attributes ? this->matches(entity, attributes->extrusion_role()) : false;
}

bool HasRoleVisitor::search(const ExtrusionEntity &entity, HasRoleVisitor&& visitor)
{
    visitor.traverse(entity);
    return visitor.found;
}

bool HasRoleVisitor::search(const ExtrusionEntitiesPtr &entities, HasRoleVisitor&& visitor)
{
    for (ExtrusionEntity *ptr : entities) {
        visitor.traverse(*ptr);
        if (visitor.found) return true;
    }
    return visitor.found;
}

void SimplifyVisitor::simplify(ExtrusionEntity &entity, coordf_t tolerance, ArcFittingType with_fitting_arc, double fitting_arc_tolerance)
{
    ArcPolyline *polyline = entity.polyline_or_null();
    const ExtrusionAttributes *attributes = entity.get_property<ExtrusionAttributes>();
    if (polyline == nullptr || attributes == nullptr)
        return;

    if (polyline->has_z_offset()) {
        polyline->make_arc(ArcFittingType::Disabled, tolerance, fitting_arc_tolerance);
        // TODO: simplify but only for sub-path with same zheight.
        return;
    }
    if (with_fitting_arc != ArcFittingType::Disabled) {
        if (attributes->extrusion_role().is_sparse_infill())
            // Use 3x lower resolution than the object fine detail for sparse infill.
            tolerance *= 3.;
        else if (attributes->extrusion_role().is_support())
            // Use 4x lower resolution than the object fine detail for support.
            tolerance *= 4.;
        else if (attributes->extrusion_role().is_skirt())
            // Brim is currently marked as skirt.
            // Use 4x lower resolution than the object fine detail for skirt & brim.
            tolerance *= 4.;
    }
    polyline->make_arc(with_fitting_arc, tolerance, fitting_arc_tolerance);
}

void SimplifyVisitor::traverse(ExtrusionEntity &entity)
{
    m_last_deleted = false;
    this->simplify_entity(entity);
}

void SimplifyVisitor::simplify_entity(ExtrusionEntity& entity) {
    const ExtrusionAttributes *entity_attributes = entity.get_property<ExtrusionAttributes>();
    const ExtrusionAttributes *attributes        = entity_attributes != nullptr ? entity_attributes : m_current_attributes;
    if (ArcPolyline *polyline = entity.polyline_or_null()) {
        assert(entity_attributes != nullptr);
        if (attributes == nullptr)
            return;

        if (m_min_path_size > 0 && entity.length() < m_min_path_size) {
            m_last_deleted = true;
            return;
        }
        assert(m_scaled_resolution >= SCALED_EPSILON);
        coordf_t tolerance = m_scaled_resolution;
        if (m_use_arc_fitting != ArcFittingType::Disabled) {
            if (attributes->extrusion_role().is_sparse_infill())
                // Use 3x lower resolution than the object fine detail for sparse infill.
                tolerance *= 3.;
            else if (attributes->extrusion_role().is_support())
                // Use 4x lower resolution than the object fine detail for support.
                tolerance *= 4.;
            else if (attributes->extrusion_role().is_skirt())
                // Brim is currently marked as skirt.
                // Use 4x lower resolution than the object fine detail for skirt & brim.
                tolerance *= 4.;
        }
        coordf_t fitting_tolerance = scale_d(m_arc_fitting_tolearance->get_effective_value(attributes->width));
        if (polyline->has_z_offset()) {
            // TODO: simplify but only for sub-path with same zheight.
            //polyline->make_arc(ArcFittingType::Disabled, tolerance, fitting_tolerance);
        } else {
            polyline->make_arc(m_use_arc_fitting, tolerance, fitting_tolerance);
        }
        // extra simplify if points are too close (unless z-profile, as they can have same position but different z)
        if (!polyline->has_z_offset()) {
            for (int i = 1; i < polyline->size(); ++i) {
                if (polyline->get_point(i - 1).coincides_with_epsilon(polyline->get_point(i))) {
                    polyline->make_arc(m_use_arc_fitting, tolerance, fitting_tolerance);
                    break;
                }
            }
            for (int i = 1; i < polyline->size(); ++i) {
                assert(!polyline->get_point(i - 1).coincides_with_epsilon(polyline->get_point(i)));
            }
        }
        return;
    }

    if (entity.is_leaf())
        return;
    if (m_ignore_holes && entity.is_loop()) {
        const ExtrusionPropertyLoopRole *loop_role_property = entity.get_property<ExtrusionPropertyLoopRole>();
        ExtrusionLoopRole loop_role = loop_role_property == nullptr ? elrDefault : loop_role_property->perimeter_role();
        if ((loop_role & elrHole) != 0)
            return;
    }

    const ExtrusionAttributes *old_current_attributes = m_current_attributes;
    if (entity_attributes != nullptr)
        m_current_attributes = entity_attributes;

    ExtrusionEntity::Children &children = entity.children();
    for (size_t i = 0; i < children.size(); ++i) {
        ExtrusionEntity *child = children[i].get();
        this->simplify_entity(*child);
        while (m_last_deleted) {
            if (!entity.is_continuous()) {
                children.erase(children.begin() + i);
                --i;
                m_last_deleted = false;
                break;
            }
            if (i > 0) {
                ArcPolyline *path = child->polyline_or_null();
                ArcPolyline *path_previous = children[i - 1]->polyline_or_null();
                assert(path != nullptr);
                assert(path_previous != nullptr);
                if (path == nullptr || path_previous == nullptr) {
                    m_current_attributes = old_current_attributes;
                    return;
                }
                path_previous->append(*path);
                children.erase(children.begin() + i);
                --i;
            } else if (i + 1 < children.size()) {
                ArcPolyline *path = child->polyline_or_null();
                ArcPolyline *path_next = children[i + 1]->polyline_or_null();
                assert(path != nullptr);
                assert(path_next != nullptr);
                if (path == nullptr || path_next == nullptr) {
                    m_current_attributes = old_current_attributes;
                    return;
                }
                path->append(*path_next);
                children.erase(children.begin() + i + 1);
            } else {
                // return, the caller need to delete me.
                m_current_attributes = old_current_attributes;
                return;
            }
            m_last_deleted = false;
            child = children[i].get();
            this->simplify_entity(*child);
        }
    }
    m_current_attributes = old_current_attributes;
}

void GetPathsVisitor::visit_leaf(ExtrusionEntity& entity)
{
    if (entity.polyline_or_null() != nullptr)
        paths.push_back(&entity);
}

void ExtrusionVolume::visit_leaf(const ExtrusionEntity &entity)
{
    const ExtrusionAttributes *attributes = entity.get_property<ExtrusionAttributes>();
    if (attributes == nullptr)
        return;
    if (entity.role() == ExtrusionRole::GapFill && !_with_gap_fill)
        return;
    volume += unscaled(entity.length()) * attributes->mm3_per_mm * _flow_ratio;
}

void ExtrusionModifyFlow::visit_leaf(ExtrusionEntity &entity)
{
    ExtrusionAttributes *attributes = entity.get_property<ExtrusionAttributes>();
    if (attributes == nullptr)
        return;
    attributes->mm3_per_mm *= _flow_mult;
    attributes->width *= _flow_mult;
}

void CreateBoundingBoxVisitor::visit_leaf(const ExtrusionEntity &entity)
{
    const ArcPolyline *polyline = entity.polyline_or_null();
    if (polyline == nullptr)
        return;
    for (const Geometry::ArcWelder::Segment &pt : polyline->get_arc())
        bb.merge(pt.point);
}

void CountEntities::visit_leaf(const ExtrusionEntity &entity)
{
    ++leaf_number;
}

void FlatenEntities::enter_node(const ExtrusionEntity &entity)
{
    if (m_skip_depth > 0) {
        ++m_skip_depth;
        return;
    }

    if (!entity.is_collection()) {
        assert(!m_output_stack.empty());
        m_output_stack.back()->append_child(entity);
        m_skip_depth = 1;
        return;
    }

    assert(!entity.is_leaf());
    const ExtrusionEntity::Children &children = entity.children();
    const bool publish_group = children.size() > 1 &&
        ((!entity.can_sort() || !m_output_stack.back()->can_sort()) && preserve_ordering);
    m_publish_group_stack.push_back(publish_group);
    if (publish_group) {
        // A non-sortable collection encodes a required print order. Flatten its
        // children into a temporary collection and publish that collection as a
        // single child so later path planning cannot reorder it accidentally.
        m_group_stack.push_back(std::make_unique<ExtrusionEntityCollection>());
        m_group_stack.back()->set_can_sort_reverse(entity.can_sort(), entity.can_reverse());
        m_output_stack.push_back(m_group_stack.back().get());
    }
}

void FlatenEntities::visit_leaf(const ExtrusionEntity&)
{
    // Leaves are appended by enter_node(). Keeping the work in one callback lets
    // the same logic preserve whole loops/multipaths without visiting their
    // child paths.
}

void FlatenEntities::leave_node(const ExtrusionEntity&)
{
    if (m_skip_depth > 0) {
        --m_skip_depth;
        return;
    }

    assert(!m_publish_group_stack.empty());
    const bool publish_group = m_publish_group_stack.back();
    m_publish_group_stack.pop_back();
    if (publish_group) {
        assert(m_output_stack.size() > 1);
        assert(!m_group_stack.empty());
        std::unique_ptr<ExtrusionEntityCollection> group = std::move(m_group_stack.back());
        m_group_stack.pop_back();
        m_output_stack.pop_back();
        m_output_stack.back()->append(std::move(group));
    }
}

ExtrusionEntityCollection&& FlatenEntities::flatten(const ExtrusionEntity &to_flatten) && {
    m_output_stack.clear();
    m_group_stack.clear();
    m_publish_group_stack.clear();
    m_skip_depth = 0;
    m_output_stack.push_back(&to_fill);
    this->traverse(to_flatten);
    m_output_stack.clear();
    return std::move(to_fill);
}

#ifdef _DEBUG
void TestCollection::enter_node(const ExtrusionEntity& entity)
{
    if (entity.is_leaf())
        return;
    for (const ExtrusionEntityUPtr &child : entity.children()) {
        assert(child);
        std::cout << "entity at " << ((uint64_t)(void*)child.get()) << "\n";
    }
}

void TestCollection::visit_leaf(const ExtrusionEntity& entity)
{
    assert(entity.as_polyline().size() > 0);
}
#endif

#ifdef _DEBUGINFO
void LoopAssertVisitor::enter_node(const ExtrusionEntity& entity)
{
    if (entity.child_count() > 0) {
        release_assert(!entity.empty());
        Point last_pt = entity.is_loop() ? entity.last_point() : entity.first_point();
        const ExtrusionEntity::Children &children = entity.children();
        for (const ExtrusionEntityUPtr &child : children) {
            if (!child)
                continue;
            if (entity.is_loop() || entity.is_continuous())
                release_assert(child->first_point() == last_pt);
            last_pt = child->last_point();
        }
        if (entity.is_loop())
            release_assert(entity.first_point() == entity.last_point());
        return;
    }
}

void LoopAssertVisitor::visit_leaf(const ExtrusionEntity& entity)
{
    const ArcPolyline *polyline = entity.polyline_or_null();
    const ExtrusionPropertyOverhang *overhang = this->current_property<ExtrusionPropertyOverhang>();
    const ExtrusionAttributes *attributes = this->current_property<ExtrusionAttributes>();
    const ExtrusionRole role = attributes != nullptr ? attributes->extrusion_role() : entity.role();
    release_assert (!role.is_overhang() || overhang != nullptr);
    if (m_check_length <= 0 || polyline == nullptr)
        return;
    release_assert(!entity.empty());
    const bool has_z_offset = polyline->has_z_offset();
    // Sawtooth support may use z-profile paths as non-extruding 3D moves.
    release_assert(attributes == nullptr || attributes->mm3_per_mm > 0.000001 || role == ExtrusionRole::Travel || has_z_offset);
    if (has_z_offset) {
        double length_3d = 0.;
        for (size_t idx = 1; idx < polyline->size(); ++idx) {
            const Point &previous_point = polyline->get_point(idx - 1);
            const Point &point          = polyline->get_point(idx);
            coord_t previous_z = polyline->z_offset(idx - 1);
            coord_t z          = polyline->z_offset(idx);
            coord_t dz         = previous_z < z ? z - previous_z : previous_z - z;
            release_assert(!previous_point.coincides_with_epsilon(point) || dz > 0);
            double xy_length = previous_point.distance_to(point);
            length_3d += std::sqrt(xy_length * xy_length + double(dz) * double(dz));
        }
        release_assert(length_3d > m_check_length);
    } else {
        release_assert(entity.length() > m_check_length);
        for (size_t idx = 1; idx < polyline->size(); ++idx)
            release_assert(!polyline->get_point(idx - 1).coincides_with_epsilon(polyline->get_point(idx)));
    }
}
#endif

} // namespace Slic3r
