///|/ Copyright (c) SuperSlicer 2026 Durand Remi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/

#include "MachineEnvelope.hpp"

#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <string>

#include "libslic3r/Api/plugin/cpp/ConfigViews.hpp"
#include "libslic3r/FFFPrintConfig.hpp"

/*
Firmware-neutral machine envelope implementation
================================================

The reader translates stable plugin Config views into a validated value
object. Keeping this operation outside concrete firmware sessions prevents
each dialect from interpreting printer settings differently.
*/

namespace slic3r_api { namespace GCodeGeneration {
namespace {

// Reject invalid limits at the configuration boundary so every firmware may
// assume that an available envelope contains usable physical values.
void validate_limit(const char *setting, double value)
{
    if (!std::isfinite(value) || value < 0.0)
        throw std::invalid_argument(
            "Machine envelope setting '" + std::string(setting) +
            "' must be a finite non-negative value.");
}

} // namespace

std::optional<MachineEnvelope> configured_machine_envelope(const Config &config)
{
    // MachineLimitsUsage is a core enum, while Config deliberately exposes a
    // stable integer through the plugin API. Compare the two at this boundary
    // so firmware implementations never depend on the enum representation.
    if (config.enum_or_default("machine_limits_usage",
                               int32_t(Slic3r::MachineLimitsUsage::TimeEstimateOnly)) !=
        int32_t(Slic3r::MachineLimitsUsage::EmitToGCode))
        return std::nullopt;

    MachineEnvelope envelope;
    envelope.max_acceleration_x = config.vector_float_or_default("machine_max_acceleration_x", 0, 0.0);
    envelope.max_acceleration_y = config.vector_float_or_default("machine_max_acceleration_y", 0, 0.0);
    envelope.max_acceleration_z = config.vector_float_or_default("machine_max_acceleration_z", 0, 0.0);
    envelope.max_acceleration_e = config.vector_float_or_default("machine_max_acceleration_e", 0, 0.0);
    envelope.max_feedrate_x = config.vector_float_or_default("machine_max_feedrate_x", 0, 0.0);
    envelope.max_feedrate_y = config.vector_float_or_default("machine_max_feedrate_y", 0, 0.0);
    envelope.max_feedrate_z = config.vector_float_or_default("machine_max_feedrate_z", 0, 0.0);
    envelope.max_feedrate_e = config.vector_float_or_default("machine_max_feedrate_e", 0, 0.0);
    envelope.max_print_acceleration = config.vector_float_or_default(
        "machine_max_acceleration_extruding", 0, 0.0);
    envelope.max_retract_acceleration = config.vector_float_or_default(
        "machine_max_acceleration_retracting", 0, 0.0);
    envelope.max_travel_acceleration = config.vector_float_or_default(
        "machine_max_acceleration_travel", 0, 0.0);
    envelope.max_jerk_x = config.vector_float_or_default("machine_max_jerk_x", 0, 0.0);
    envelope.max_jerk_y = config.vector_float_or_default("machine_max_jerk_y", 0, 0.0);
    envelope.max_jerk_z = config.vector_float_or_default("machine_max_jerk_z", 0, 0.0);
    envelope.max_jerk_e = config.vector_float_or_default("machine_max_jerk_e", 0, 0.0);
    envelope.min_travel_feedrate = config.vector_float_or_default("machine_min_travel_rate", 0, 0.0);
    envelope.min_extruding_feedrate = config.vector_float_or_default(
        "machine_min_extruding_rate", 0, 0.0);

    // Validate every copied field once. Concrete encoders are then concerned
    // only with syntax, unit conversion and representable command subsets.
    validate_limit("machine_max_acceleration_x", envelope.max_acceleration_x);
    validate_limit("machine_max_acceleration_y", envelope.max_acceleration_y);
    validate_limit("machine_max_acceleration_z", envelope.max_acceleration_z);
    validate_limit("machine_max_acceleration_e", envelope.max_acceleration_e);
    validate_limit("machine_max_feedrate_x", envelope.max_feedrate_x);
    validate_limit("machine_max_feedrate_y", envelope.max_feedrate_y);
    validate_limit("machine_max_feedrate_z", envelope.max_feedrate_z);
    validate_limit("machine_max_feedrate_e", envelope.max_feedrate_e);
    validate_limit("machine_max_acceleration_extruding", envelope.max_print_acceleration);
    validate_limit("machine_max_acceleration_retracting", envelope.max_retract_acceleration);
    validate_limit("machine_max_acceleration_travel", envelope.max_travel_acceleration);
    validate_limit("machine_max_jerk_x", envelope.max_jerk_x);
    validate_limit("machine_max_jerk_y", envelope.max_jerk_y);
    validate_limit("machine_max_jerk_z", envelope.max_jerk_z);
    validate_limit("machine_max_jerk_e", envelope.max_jerk_e);
    validate_limit("machine_min_travel_rate", envelope.min_travel_feedrate);
    validate_limit("machine_min_extruding_rate", envelope.min_extruding_feedrate);
    return envelope;
}

}} // namespace slic3r_api::GCodeGeneration
