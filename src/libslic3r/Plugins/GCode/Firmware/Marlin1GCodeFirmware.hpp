///|/ Copyright (c) SuperSlicer 2026 Durand Remi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/
#ifndef slic3r_Plugins_GCode_Firmware_Marlin1GCodeFirmware_hpp_
#define slic3r_Plugins_GCode_Firmware_Marlin1GCodeFirmware_hpp_

#include "libslic3r/Api/plugin/cpp/gcode/MachineEnvelope.hpp"
#include "libslic3r/Api/plugin/cpp/gcode/SingleAccelerationRegisterGCodeFirmwareSession.hpp"

/*
Marlin 1 firmware session
=========================

Marlin 1 reuses the standard extrusion traversal and machine states. Its
single-register base session models M204 S by invalidating the opposite print
or travel acceleration acknowledgement in Gantry.
*/

namespace slic3r_api { namespace GCodeGeneration { namespace Firmware {

class Marlin1GCodeFirmwareSession :
    public SingleAccelerationRegisterGCodeFirmwareSession
{
protected:
    std::string encode_machine_envelope(const MachineEnvelope &envelope) const override;
    void synchronize_selected_extruder_state(const DefaultExtruder *previous,
                                              DefaultExtruder &selected) override;
    std::string encode_tool_temperature(uint16_t tool_id,
                                        int16_t temperature,
                                        bool wait) const override;
    std::string encode_acceleration(uint32_t acceleration, bool travel) const override;
};

}}} // namespace slic3r_api::GCodeGeneration::Firmware

#endif // slic3r_Plugins_GCode_Firmware_Marlin1GCodeFirmware_hpp_
