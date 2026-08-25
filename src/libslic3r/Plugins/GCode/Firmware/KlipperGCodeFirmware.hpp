///|/ Copyright (c) SuperSlicer 2026 Durand Remi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/
#ifndef slic3r_Plugins_GCode_Firmware_KlipperGCodeFirmware_hpp_
#define slic3r_Plugins_GCode_Firmware_KlipperGCodeFirmware_hpp_

#include <string>
#include <vector>

#include "libslic3r/Api/plugin/cpp/gcode/MachineEnvelope.hpp"
#include "libslic3r/Api/plugin/cpp/gcode/SingleAccelerationRegisterGCodeFirmwareSession.hpp"

/*
Klipper firmware session
========================

Klipper stores its configured extruder names inside the session. The standard
single-register session models its M204 register without a Klipper-specific
state cache.
*/

namespace slic3r_api { namespace GCodeGeneration { namespace Firmware {

class KlipperGCodeFirmwareSession :
    public SingleAccelerationRegisterGCodeFirmwareSession
{
public:
    using SingleAccelerationRegisterGCodeFirmwareSession::SingleAccelerationRegisterGCodeFirmwareSession;

protected:
    std::string encode_machine_envelope(const MachineEnvelope &envelope) const override;
    void setup_firmware(const Config &config) override;
    void synchronize_selected_extruder_state(const DefaultExtruder *previous,
                                              DefaultExtruder &selected) override;
    std::string encode_tool_change(uint16_t tool_id) const override;
    std::string encode_tool_temperature(uint16_t tool_id,
                                        int16_t temperature,
                                        bool wait) const override;
    std::string encode_chamber_temperature(int16_t temperature, bool wait) const override;
    std::string encode_pressure_advance(uint16_t tool_id,
                                        double pressure_advance) const override;
    std::string encode_acceleration(uint32_t acceleration, bool travel) const override;
    std::string encode_move(const PreparedMove &move, bool include_speed) const override;
    std::string encode_extruder_current(uint16_t tool_id, double current) const override;

private:
    const std::string &tool_name(uint16_t tool_id) const;

    std::vector<std::string> m_tool_names;
};

}}} // namespace slic3r_api::GCodeGeneration::Firmware

#endif // slic3r_Plugins_GCode_Firmware_KlipperGCodeFirmware_hpp_
