///|/ Copyright (c) SuperSlicer 2026 Durand Remi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/

#include "GCodeScriptExecutionContext.hpp"

#include <algorithm>
#include <stdexcept>
#include <vector>

/*
Final PrintingPlan script index
===============================

The implementation flattens groups, layers and tool sections once, then stores
the scalar neighbours of every before/after event root. Serialization itself
only performs map lookups and updates the current scalar position.
*/

namespace slic3r_api { namespace GCodeGeneration {
namespace {

struct IndexedGroup
{
    std::optional<uint32_t> object;
    size_t first_layer = 0;
    size_t layer_count = 0;
    size_t first_tool = 0;
    size_t tool_count = 0;
};

struct IndexedLayer
{
    GCodeScriptExecutionContext::LayerPosition position;
    size_t group_idx = 0;
    size_t first_tool = 0;
    size_t tool_count = 0;
};

struct IndexedTool
{
    uint16_t extruder_id = uint16_t(-1);
};

std::optional<uint32_t> object_index(const Print &print, const Object &object);
std::optional<uint32_t> previous_object(const std::vector<IndexedGroup> &groups, size_t boundary);
std::optional<uint32_t> next_object(const std::vector<IndexedGroup> &groups, size_t boundary);

std::optional<uint32_t> object_index(const Print &print, const Object &object)
{
    if (!object.valid())
        return std::nullopt;
    for (uint32_t idx = 0; idx < print.object_count(); ++idx)
        if (print.object(idx).handle() == object.handle())
            return idx;
    return std::nullopt;
}

std::optional<uint32_t> previous_object(const std::vector<IndexedGroup> &groups, size_t boundary)
{
    for (size_t idx = std::min(boundary, groups.size()); idx > 0; --idx)
        if (groups[idx - 1].object)
            return groups[idx - 1].object;
    return std::nullopt;
}

std::optional<uint32_t> next_object(const std::vector<IndexedGroup> &groups, size_t boundary)
{
    for (size_t idx = std::min(boundary, groups.size()); idx < groups.size(); ++idx)
        if (groups[idx].object)
            return groups[idx].object;
    return std::nullopt;
}

} // namespace

void GCodeScriptExecutionContext::initialize(const Print &print, const PrintingPlan &plan)
{
    m_event_positions.clear();
    m_active_event.reset();
    m_max_print_z = 0;
    m_first_extruder.reset();
    m_last_extruder.reset();
    m_current_object.reset();
    m_current_layer.reset();
    m_previous_layer.reset();
    m_current_extruder.reset();
    m_previous_extruder.reset();

    std::vector<IndexedGroup> groups;
    std::vector<IndexedLayer> layers;
    std::vector<IndexedTool> tools;
    groups.reserve(plan.group_count());

    // Flatten the immutable final hierarchy so every event root can be
    // described by simple indices instead of borrowed child views.
    for (uint32_t group_idx = 0; group_idx < plan.group_count(); ++group_idx) {
        const PrintingGroup group = plan.group(group_idx);
        IndexedGroup indexed_group;
        if (group.object_instance_count() == 1)
            indexed_group.object = object_index(print, group.object_instance(0).object());
        indexed_group.first_layer = layers.size();
        indexed_group.first_tool = tools.size();

        for (uint32_t layer_idx = 0; layer_idx < group.layer_group_count(); ++layer_idx) {
            const PrintingLayerGroup layer = group.layer_group(layer_idx);
            IndexedLayer indexed_layer;
            indexed_layer.position.number = static_cast<int32_t>(layers.size());
            indexed_layer.position.print_z = layer.print_z();
            indexed_layer.group_idx = group_idx;
            indexed_layer.first_tool = tools.size();
            m_max_print_z = std::max(m_max_print_z, layer.print_z());

            for (uint32_t tool_idx = 0; tool_idx < layer.tool_group_count(); ++tool_idx) {
                const uint16_t extruder_id = layer.tool_group(tool_idx).extruder_id();
                tools.push_back(IndexedTool{extruder_id});
                if (extruder_id != uint16_t(-1)) {
                    if (!m_first_extruder)
                        m_first_extruder = extruder_id;
                    m_last_extruder = extruder_id;
                }
            }
            indexed_layer.tool_count = tools.size() - indexed_layer.first_tool;
            layers.push_back(indexed_layer);
        }
        indexed_group.layer_count = layers.size() - indexed_group.first_layer;
        indexed_group.tool_count = tools.size() - indexed_group.first_tool;
        groups.push_back(indexed_group);
    }

    const auto make_position = [&groups, &layers, &tools](size_t group_boundary,
                                                          size_t layer_boundary,
                                                          size_t tool_boundary) {
        EventPosition position;
        position.previous_object = previous_object(groups, group_boundary);
        position.next_object = next_object(groups, group_boundary);
        if (layer_boundary > 0 && layer_boundary <= layers.size())
            position.previous_layer = layers[layer_boundary - 1].position;
        if (layer_boundary < layers.size())
            position.next_layer = layers[layer_boundary].position;
        if (tool_boundary > 0 && tool_boundary <= tools.size() &&
            tools[tool_boundary - 1].extruder_id != uint16_t(-1))
            position.previous_extruder = tools[tool_boundary - 1].extruder_id;
        if (tool_boundary < tools.size() && tools[tool_boundary].extruder_id != uint16_t(-1))
            position.next_extruder = tools[tool_boundary].extruder_id;
        return position;
    };
    const auto register_events = [this, &make_position](const PrintingScopeEvents &events,
                                                        size_t before_group,
                                                        size_t after_group,
                                                        size_t before_layer,
                                                        size_t after_layer,
                                                        size_t before_tool,
                                                        size_t after_tool) {
        m_event_positions[events.before().handle()] =
            make_position(before_group, before_layer, before_tool);
        m_event_positions[events.after().handle()] =
            make_position(after_group, after_layer, after_tool);
    };

    // Register every fixed scope root. The boundary immediately before a
    // scope points to its first descendant; the boundary after it points to
    // the first descendant of the following scope.
    register_events(plan.events(), 0, groups.size(), 0, layers.size(), 0, tools.size());
    for (uint32_t group_idx = 0; group_idx < plan.group_count(); ++group_idx) {
        const PrintingGroup group = plan.group(group_idx);
        const IndexedGroup &indexed_group = groups[group_idx];
        register_events(group.events(), group_idx, group_idx + 1,
                        indexed_group.first_layer,
                        indexed_group.first_layer + indexed_group.layer_count,
                        indexed_group.first_tool,
                        indexed_group.first_tool + indexed_group.tool_count);

        for (uint32_t local_layer_idx = 0; local_layer_idx < group.layer_group_count(); ++local_layer_idx) {
            const size_t layer_idx = indexed_group.first_layer + local_layer_idx;
            const PrintingLayerGroup layer = group.layer_group(local_layer_idx);
            const IndexedLayer &indexed_layer = layers[layer_idx];
            register_events(layer.events(), group_idx, group_idx + 1,
                            layer_idx, layer_idx + 1,
                            indexed_layer.first_tool,
                            indexed_layer.first_tool + indexed_layer.tool_count);

            for (uint32_t local_tool_idx = 0; local_tool_idx < layer.tool_group_count(); ++local_tool_idx) {
                const size_t tool_idx = indexed_layer.first_tool + local_tool_idx;
                const PrintingToolGroup tool_group = layer.tool_group(local_tool_idx);
                register_events(tool_group.events(), group_idx, group_idx + 1,
                                layer_idx, layer_idx + 1, tool_idx, tool_idx + 1);
            }
        }
    }
}

const GCodeScriptExecutionContext::EventPosition &
GCodeScriptExecutionContext::position_for_before(const PrintingScopeEvents &events) const
{
    const std::unordered_map<const extrusion_entity_handle *, EventPosition>::const_iterator found =
        m_event_positions.find(events.before().handle());
    if (found == m_event_positions.end())
        throw std::invalid_argument("A PrintingPlan scope is absent from the script execution index.");
    return found->second;
}

void GCodeScriptExecutionContext::begin_group(const PrintingGroup &group)
{
    const EventPosition &position = position_for_before(group.events());
    m_current_object = position.next_object;
}

void GCodeScriptExecutionContext::begin_layer(const PrintingLayerGroup &layer)
{
    const EventPosition &position = position_for_before(layer.events());
    m_previous_layer = position.previous_layer;
    m_current_layer = position.next_layer;
}

void GCodeScriptExecutionContext::begin_tool_group(const PrintingToolGroup &tool_group)
{
    const EventPosition &position = position_for_before(tool_group.events());
    m_previous_extruder = position.previous_extruder;
    m_current_extruder = position.next_extruder;
    if (!m_current_extruder && tool_group.extruder_id() != uint16_t(-1))
        m_current_extruder = tool_group.extruder_id();
}

void GCodeScriptExecutionContext::activate_event(const ExtrusionEntity &event_root)
{
    const std::unordered_map<const extrusion_entity_handle *, EventPosition>::const_iterator found =
        m_event_positions.find(event_root.handle());
    if (found == m_event_positions.end())
        throw std::invalid_argument("A G-code event root is absent from the final PrintingPlan index.");
    m_active_event = found->second;
}

void GCodeScriptExecutionContext::clear_event()
{
    m_active_event.reset();
}

std::optional<uint32_t> GCodeScriptExecutionContext::event_previous_object() const
{
    return m_active_event ? m_active_event->previous_object : std::nullopt;
}

std::optional<uint32_t> GCodeScriptExecutionContext::event_next_object() const
{
    return m_active_event ? m_active_event->next_object : std::nullopt;
}

std::optional<GCodeScriptExecutionContext::LayerPosition>
GCodeScriptExecutionContext::event_previous_layer() const
{
    return m_active_event ? m_active_event->previous_layer : std::nullopt;
}

std::optional<GCodeScriptExecutionContext::LayerPosition>
GCodeScriptExecutionContext::event_next_layer() const
{
    return m_active_event ? m_active_event->next_layer : std::nullopt;
}

std::optional<uint16_t> GCodeScriptExecutionContext::event_previous_extruder() const
{
    return m_active_event ? m_active_event->previous_extruder : std::nullopt;
}

std::optional<uint16_t> GCodeScriptExecutionContext::event_next_extruder() const
{
    return m_active_event ? m_active_event->next_extruder : std::nullopt;
}

}} // namespace slic3r_api::GCodeGeneration
