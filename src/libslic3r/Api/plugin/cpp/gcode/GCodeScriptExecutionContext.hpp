///|/ Copyright (c) SuperSlicer 2026 Durand Remi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/
#ifndef slic3r_Api_plugin_cpp_gcode_GCodeScriptExecutionContext_hpp_
#define slic3r_Api_plugin_cpp_gcode_GCodeScriptExecutionContext_hpp_

#include <cstdint>
#include <optional>
#include <unordered_map>

#include "libslic3r/Api/plugin/cpp/PrintingPlanViews.hpp"

/*
Runtime position of G-code scripts
==================================

GCodeScriptExecutionContext indexes the final PrintingPlan immediately before
serialization. It converts the plan hierarchy into stable scalar facts such as
the current layer, the adjacent object groups and the tools on either side of
an event boundary.

The class never owns or retains a PrintingPlan view. Event roots are used only
as stable lookup keys while the immutable plan is serialized. This lets a
firmware build PlaceholderParser inputs at the exact execution point instead
of trusting metadata captured when a script was inserted earlier in the
pipeline.
*/

namespace slic3r_api { namespace GCodeGeneration {

class GCodeScriptExecutionContext
{
public:
    // Identifies one layer in the final global traversal order. print_z keeps
    // the scaled plan value so callers choose when to convert it to millimetres.
    struct LayerPosition
    {
        int32_t number = -1;
        coord_t print_z = 0;
    };

    void initialize(const Print &print, const PrintingPlan &plan);

    // Scope entry methods select the scalar position matching the plan item
    // which the file writer has just entered.
    void begin_group(const PrintingGroup &group);
    void begin_layer(const PrintingLayerGroup &layer);
    void begin_tool_group(const PrintingToolGroup &tool_group);

    // write_event() temporarily selects one preindexed boundary. Clearing it
    // restores normal use of the current group/layer/tool position.
    void activate_event(const ExtrusionEntity &event_root);
    void clear_event();

    coord_t max_print_z() const { return m_max_print_z; }
    std::optional<uint16_t> first_extruder() const { return m_first_extruder; }
    std::optional<uint16_t> last_extruder() const { return m_last_extruder; }

    std::optional<uint32_t> current_object() const { return m_current_object; }
    std::optional<LayerPosition> current_layer() const { return m_current_layer; }
    std::optional<LayerPosition> previous_layer() const { return m_previous_layer; }
    std::optional<uint16_t> current_extruder() const { return m_current_extruder; }
    std::optional<uint16_t> previous_extruder() const { return m_previous_extruder; }

    bool has_active_event() const { return m_active_event.has_value(); }
    std::optional<uint32_t> event_previous_object() const;
    std::optional<uint32_t> event_next_object() const;
    std::optional<LayerPosition> event_previous_layer() const;
    std::optional<LayerPosition> event_next_layer() const;
    std::optional<uint16_t> event_previous_extruder() const;
    std::optional<uint16_t> event_next_extruder() const;

private:
    struct EventPosition
    {
        std::optional<uint32_t> previous_object;
        std::optional<uint32_t> next_object;
        std::optional<LayerPosition> previous_layer;
        std::optional<LayerPosition> next_layer;
        std::optional<uint16_t> previous_extruder;
        std::optional<uint16_t> next_extruder;
    };

    const EventPosition &position_for_before(const PrintingScopeEvents &events) const;

    std::unordered_map<const extrusion_entity_handle *, EventPosition> m_event_positions;
    std::optional<EventPosition> m_active_event;
    coord_t m_max_print_z = 0;
    std::optional<uint16_t> m_first_extruder;
    std::optional<uint16_t> m_last_extruder;
    std::optional<uint32_t> m_current_object;
    std::optional<LayerPosition> m_current_layer;
    std::optional<LayerPosition> m_previous_layer;
    std::optional<uint16_t> m_current_extruder;
    std::optional<uint16_t> m_previous_extruder;
};

}} // namespace slic3r_api::GCodeGeneration

#endif // slic3r_Api_plugin_cpp_gcode_GCodeScriptExecutionContext_hpp_
