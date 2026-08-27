///|/ Copyright (c) SuperSlicer 2026 Durand Remi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/
#ifndef slic3r_Plugins_GCode_Firmware_SprinterGCodeFirmware_hpp_
#define slic3r_Plugins_GCode_Firmware_SprinterGCodeFirmware_hpp_

#include "libslic3r/Api/plugin/cpp/gcode/MachineEnvelope.hpp"
#include "libslic3r/Api/plugin/cpp/gcode/SingleAccelerationRegisterGCodeFirmwareSession.hpp"

/*
Sprinter firmware session
=========================

Sprinter uses one effective acceleration command. Gantry keeps the independent
print and travel requests while the single-register session invalidates the
opposite acknowledgement after every M204 P command.
*/

namespace slic3r_api { namespace GCodeGeneration { namespace Firmware {

class SprinterGCodeFirmwareSession :
    public SingleAccelerationRegisterGCodeFirmwareSession
{
public:
    using SingleAccelerationRegisterGCodeFirmwareSession::SingleAccelerationRegisterGCodeFirmwareSession;

protected:
    std::string resolve_empty_script(gcode_script_type script_type,
                                     const Config *producer_config) const override;
    std::string encode_machine_envelope(const MachineEnvelope &envelope) const override;
    void synchronize_selected_extruder_state(const DefaultExtruder *previous,
                                              DefaultExtruder &selected) override;
    std::string encode_chamber_temperature(int16_t temperature, bool wait) const override;
    std::string encode_pressure_advance(uint16_t tool_id,
                                        double pressure_advance) const override;
    std::string encode_acceleration(uint32_t acceleration, bool travel) const override;
    std::string encode_extruder_current(uint16_t tool_id, double current) const override;
};

}}} // namespace slic3r_api::GCodeGeneration::Firmware

#endif // slic3r_Plugins_GCode_Firmware_SprinterGCodeFirmware_hpp_
