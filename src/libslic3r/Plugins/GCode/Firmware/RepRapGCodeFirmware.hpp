///|/ Copyright (c) SuperSlicer 2026 Durand Remi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/
#ifndef slic3r_Plugins_GCode_Firmware_RepRapGCodeFirmware_hpp_
#define slic3r_Plugins_GCode_Firmware_RepRapGCodeFirmware_hpp_

#include "libslic3r/Api/plugin/cpp/gcode/DefaultGCodeFirmwareSession.hpp"
#include "libslic3r/Api/plugin/cpp/gcode/MachineEnvelope.hpp"

/*
RepRapFirmware session
======================

RepRapFirmware reuses the generic independent print/travel motion states but
encodes tool heaters, fans and pressure advance with Duet-compatible commands.
*/

namespace slic3r_api { namespace GCodeGeneration { namespace Firmware {

class RepRapGCodeFirmwareSession : public DefaultGCodeFirmwareSession
{
public:
    using DefaultGCodeFirmwareSession::DefaultGCodeFirmwareSession;

protected:
    std::string encode_machine_envelope(const MachineEnvelope &envelope) const override;
    std::string encode_tool_temperature(uint16_t tool_id,
                                        int16_t temperature,
                                        bool wait) const override;
    std::string encode_fan(uint16_t tool_id, double speed_percent) const override;
    std::string encode_pressure_advance(uint16_t tool_id,
                                        double pressure_advance) const override;
};

}}} // namespace slic3r_api::GCodeGeneration::Firmware

#endif // slic3r_Plugins_GCode_Firmware_RepRapGCodeFirmware_hpp_
