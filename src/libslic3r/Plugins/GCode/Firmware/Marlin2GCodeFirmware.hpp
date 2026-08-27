///|/ Copyright (c) SuperSlicer 2026 Durand Remi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/
#ifndef slic3r_Plugins_GCode_Firmware_Marlin2GCodeFirmware_hpp_
#define slic3r_Plugins_GCode_Firmware_Marlin2GCodeFirmware_hpp_

#include "libslic3r/Api/plugin/cpp/gcode/DefaultGCodeFirmwareSession.hpp"
#include "libslic3r/Api/plugin/cpp/gcode/MachineEnvelope.hpp"

/*
Marlin 2 firmware session
=========================

Marlin 2 is the reference built-in dialect. It inherits the standard P/T
acceleration, movement and process encoders and only specializes temperature
waiting so cooling waits use Marlin's R parameter.
*/

namespace slic3r_api { namespace GCodeGeneration { namespace Firmware {

class Marlin2GCodeFirmwareSession : public DefaultGCodeFirmwareSession
{
public:
    using DefaultGCodeFirmwareSession::DefaultGCodeFirmwareSession;

protected:
    std::string resolve_empty_script(gcode_script_type script_type,
                                     const Config *producer_config) const override;
    std::string encode_machine_envelope(const MachineEnvelope &envelope) const override;
    void synchronize_selected_extruder_state(const DefaultExtruder *previous,
                                              DefaultExtruder &selected) override;
    std::string encode_tool_temperature(uint16_t tool_id,
                                        int16_t temperature,
                                        bool wait) const override;
};

}}} // namespace slic3r_api::GCodeGeneration::Firmware

#endif // slic3r_Plugins_GCode_Firmware_Marlin2GCodeFirmware_hpp_
