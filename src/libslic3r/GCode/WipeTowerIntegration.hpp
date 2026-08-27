///|/ Copyright (c) SuperSlicer 2026 Durand R?mi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/

#ifndef slic3r_GCode_WipeTowerIntegration_hpp_
#define slic3r_GCode_WipeTowerIntegration_hpp_

#include <string>
#include <vector>

#include "libslic3r/Config/FFFPrintConfig.hpp"

#include "WipeTower.hpp"

namespace Slic3r {

class GCodeGenerator;

namespace GCode {

class WipeTowerIntegration {
public:
    WipeTowerIntegration(
        const PrintConfig                                           &print_config,
        const PrintObjectConfig                                     &object_config,
        const std::vector<WipeTower::ToolChangeResult>              &priming,
        const std::vector<std::vector<WipeTower::ToolChangeResult>> &tool_changes,
        const WipeTower::ToolChangeResult                           &final_purge) :
        m_left(/*float(print_config.wipe_tower_x.value)*/ 0.f),
        m_right(float(/*print_config.wipe_tower_x.value +*/ object_config.wipe_tower_width.value)),
        m_wipe_tower_pos(float(object_config.wipe_tower_x.value), float(object_config.wipe_tower_y.value)),
        m_wipe_tower_rotation(float(object_config.wipe_tower_rotation_angle)),
        m_extruder_offsets(print_config.extruder_offset.get_values()),
        m_priming(priming),
        m_tool_changes(tool_changes),
        m_final_purge(final_purge),
        m_layer_idx(-1),
        m_tool_change_idx(0),
        m_last_wipe_tower_print_z(print_config.z_offset.value)
    {}

    std::string prime(GCodeGenerator &gcodegen);
    void next_layer() { ++ m_layer_idx; m_tool_change_idx = 0; }
    std::string tool_change(GCodeGenerator &gcodegen, int extruder_id, bool finish_layer);
    std::string finalize(GCodeGenerator &gcodegen);
    std::vector<float> used_filament_length() const;
    int get_current_layer_idx() const { return m_layer_idx; }
    // force travel because of delayed Z travel
    void set_force_travel(bool force_travel) { m_has_force_travel = force_travel; }

private:
    WipeTowerIntegration& operator=(const WipeTowerIntegration&);
    std::string append_tcr(GCodeGenerator &gcodegen, const WipeTower::ToolChangeResult &tcr, int new_extruder_id, double z = -1., bool need_ensure_z = true) const;

    // Postprocesses gcode: rotates and moves G1 extrusions and returns result
    std::string post_process_wipe_tower_moves(const WipeTower::ToolChangeResult &tcr,
                                              const Vec2f &translation,
                                              float angle,
                                              //const GCodeFlavor gcode_flavor,
                                              GCodeGenerator &gcodegen,
                                              int new_extruder_id
                                              ) const;
    std::string deretraction_from_wipe_tower_generator(GCodeGenerator &gcodegen, const WipeTower::ToolChangeResult& tcr, int new_extruder_id) const;
    std::string toolchange_gcode_from_wipe_tower_generator(GCodeGenerator &gcodegen, const WipeTower::ToolChangeResult& tcr, int new_extruder_id) const;

    // Left / right edges of the wipe tower, for the planning of wipe moves.
    const float                                                  m_left;
    const float                                                  m_right;
    const Vec2f                                                  m_wipe_tower_pos;
    const float                                                  m_wipe_tower_rotation;
    const std::vector<Vec2d>                                     m_extruder_offsets;

    // Reference to cached values at the Printer class.
    const std::vector<WipeTower::ToolChangeResult>              &m_priming;
    const std::vector<std::vector<WipeTower::ToolChangeResult>> &m_tool_changes;
    const WipeTower::ToolChangeResult                           &m_final_purge;
    // Current layer index.
    int                                                          m_layer_idx;
    int                                                          m_tool_change_idx;
    double                                                       m_last_wipe_tower_print_z;

    bool                                                         m_has_force_travel = false;
};

} // namespace GCode
} // namespace Slic3r

#endif // slic3r_GCode_WipeTowerIntegration_hpp_ 
