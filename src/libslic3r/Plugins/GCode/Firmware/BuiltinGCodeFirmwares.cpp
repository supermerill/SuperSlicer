///|/ Copyright (c) SuperSlicer 2026 Durand Remi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/

#include "BuiltinGCodeFirmwares.hpp"

#include <algorithm>
#include <cstddef>
#include <iterator>
#include <memory>

#include "KlipperGCodeFirmware.hpp"
#include "Marlin1GCodeFirmware.hpp"
#include "Marlin2GCodeFirmware.hpp"
#include "PrusaGCodeFirmware.hpp"
#include "RepRapGCodeFirmware.hpp"
#include "SprinterGCodeFirmware.hpp"
#include "libslic3r/Api/plugin/c/steps/slic3r_step_gcode_firmware.h"
#include "libslic3r/Api/plugin/cpp/PluginBase.hpp"
#include "libslic3r/Api/plugin/cpp/gcode/GCodeScriptProcessorViews.hpp"

/*
Built-in G-code firmware provider implementation
================================================

One small provider adapter owns the metadata common to all built-in dialects.
Each run still creates a concrete, independent session so no machine state can
leak between exports.
*/

namespace slic3r_api { namespace GCodeGeneration { namespace Firmware {
namespace {

typedef std::unique_ptr<GCodeFirmwareSession> (*FirmwareSessionFactory)(
    GCodeScriptProcessorView scripts,
    storage_handle *storage);

struct FirmwareDefinition
{
    const char *id;
    const char *name;
    const char *description;
    int32_t priority;
    FirmwareSessionFactory factory;
    const raw_used_config_key *extra_used_config_keys;
    size_t extra_used_config_key_count;
    bool registers_ui;
};

template<class SessionType>
std::unique_ptr<GCodeFirmwareSession> create_session(GCodeScriptProcessorView scripts,
                                                     storage_handle *storage)
{
    return std::unique_ptr<GCodeFirmwareSession>(new SessionType(scripts, storage));
}

const char *const k_no_dependencies[] = { nullptr };

const raw_used_config_key k_common_used_config_keys[] = {
    {"deretract_speed", RAW_CO_VECTOR_FLOAT, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE},
    {"extruder_fan_offset", RAW_CO_VECTOR_PERCENT, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE},
    {"extruder_offset", RAW_CO_VECTOR_POINT, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE},
    {"extruder_temperature_offset", RAW_CO_VECTOR_FLOAT, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE},
    {"extrusion_multiplier", RAW_CO_VECTOR_FLOAT, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE},
    {"filament_diameter", RAW_CO_VECTOR_FLOAT, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE},
    {"gcode_precision_e", RAW_CO_INT, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE},
    {"gcode_precision_xyz", RAW_CO_INT, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE},
    {"machine_limits_usage", RAW_CO_ENUM, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE},
    {"machine_max_acceleration_e", RAW_CO_VECTOR_FLOAT, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE},
    {"machine_max_acceleration_extruding", RAW_CO_VECTOR_FLOAT, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE},
    {"machine_max_acceleration_retracting", RAW_CO_VECTOR_FLOAT, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE},
    {"machine_max_acceleration_travel", RAW_CO_VECTOR_FLOAT, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE},
    {"machine_max_acceleration_x", RAW_CO_VECTOR_FLOAT, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE},
    {"machine_max_acceleration_y", RAW_CO_VECTOR_FLOAT, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE},
    {"machine_max_acceleration_z", RAW_CO_VECTOR_FLOAT, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE},
    {"machine_max_feedrate_e", RAW_CO_VECTOR_FLOAT, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE},
    {"machine_max_feedrate_x", RAW_CO_VECTOR_FLOAT, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE},
    {"machine_max_feedrate_y", RAW_CO_VECTOR_FLOAT, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE},
    {"machine_max_feedrate_z", RAW_CO_VECTOR_FLOAT, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE},
    {"machine_max_jerk_e", RAW_CO_VECTOR_FLOAT, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE},
    {"machine_max_jerk_x", RAW_CO_VECTOR_FLOAT, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE},
    {"machine_max_jerk_y", RAW_CO_VECTOR_FLOAT, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE},
    {"machine_max_jerk_z", RAW_CO_VECTOR_FLOAT, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE},
    {"machine_min_extruding_rate", RAW_CO_VECTOR_FLOAT, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE},
    {"machine_min_travel_rate", RAW_CO_VECTOR_FLOAT, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE},
    {"nozzle_diameter", RAW_CO_VECTOR_FLOAT, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE},
    {"retract_lift", RAW_CO_VECTOR_FLOAT, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE},
    {"retract_speed", RAW_CO_VECTOR_FLOAT, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE},
    {"travel_speed", RAW_CO_FLOAT, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE},
    {"use_firmware_retraction", RAW_CO_BOOL, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE},
    {"use_relative_e_distances", RAW_CO_BOOL, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE},
    {"use_volumetric_e", RAW_CO_BOOL, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE},
    {"z_offset", RAW_CO_FLOAT, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE}
};

const raw_used_config_key k_klipper_used_config_keys[] = {
    {"tool_name", RAW_CO_VECTOR_STRING, RAW_CONTAINER_TYPE_NONE, RAW_PRESET_TYPE_NONE}
};

const FirmwareDefinition k_marlin2_definition = {
    "gcode.firmware.marlin2",
    "Marlin 2",
    "G-code firmware session for Marlin 2.",
    0,
    &create_session<Marlin2GCodeFirmwareSession>,
    nullptr,
    0,
    true
};

const FirmwareDefinition k_marlin1_definition = {
    "gcode.firmware.marlin1",
    "Marlin 1",
    "G-code firmware session for Marlin 1.",
    10,
    &create_session<Marlin1GCodeFirmwareSession>,
    nullptr,
    0,
    false
};

const FirmwareDefinition k_prusa_definition = {
    "gcode.firmware.prusa",
    "Prusa firmware",
    "G-code firmware session for Prusa firmware.",
    20,
    &create_session<PrusaGCodeFirmwareSession>,
    nullptr,
    0,
    false
};

const FirmwareDefinition k_reprap_definition = {
    "gcode.firmware.reprap",
    "RepRapFirmware",
    "G-code firmware session for RepRapFirmware.",
    30,
    &create_session<RepRapGCodeFirmwareSession>,
    nullptr,
    0,
    false
};

const FirmwareDefinition k_sprinter_definition = {
    "gcode.firmware.sprinter",
    "Sprinter",
    "G-code firmware session for Sprinter.",
    40,
    &create_session<SprinterGCodeFirmwareSession>,
    nullptr,
    0,
    false
};

const FirmwareDefinition k_klipper_definition = {
    "gcode.firmware.klipper",
    "Klipper",
    "G-code firmware session for Klipper.",
    50,
    &create_session<KlipperGCodeFirmwareSession>,
    k_klipper_used_config_keys,
    std::size(k_klipper_used_config_keys),
    false
};

class BuiltinGCodeFirmwarePlugin final : public PluginBase
{
public:
    BuiltinGCodeFirmwarePlugin(orchestrator_handle *orchestrator,
                               const FirmwareDefinition &definition) :
        PluginBase(orchestrator),
        m_definition(definition)
    {}

private:
    const char *id_impl() const noexcept override { return m_definition.id; }
    const char *name_impl() const noexcept override { return m_definition.name; }
    const char *description_impl() const noexcept override { return m_definition.description; }
    const char *exclusive_group_impl() const noexcept override { return "gcode_firmware_plugin"; }
    const char *exclusive_group_label_impl() const noexcept override { return "G-code firmware"; }
    const char *exclusive_group_tooltip_impl() const noexcept override
    {
        return "Selects the firmware session used by PrintingPlan output writers.";
    }
    slicing_step_t step_impl() const noexcept override { return GCODE_FIRMWARE; }
    const char *const *dependencies_impl() const noexcept override { return k_no_dependencies; }
    int32_t priority_impl() const noexcept override { return m_definition.priority; }
    int32_t used_config_keys(raw_used_config_key *keys) const noexcept override;
    void inilialize_impl(storage_handle *storage) const override;
    void run_impl(const plugin_run_context *run_ctx) const override;

    const FirmwareDefinition &m_definition;
};

int32_t BuiltinGCodeFirmwarePlugin::used_config_keys(raw_used_config_key *keys) const noexcept
{
    const size_t common_count = std::size(k_common_used_config_keys);
    if (keys != nullptr) {
        std::copy(std::begin(k_common_used_config_keys), std::end(k_common_used_config_keys), keys);
        if (m_definition.extra_used_config_keys != nullptr)
            std::copy_n(m_definition.extra_used_config_keys,
                        m_definition.extra_used_config_key_count,
                        keys + common_count);
    }
    return int32_t(common_count + m_definition.extra_used_config_key_count);
}

void BuiltinGCodeFirmwarePlugin::inilialize_impl(storage_handle *) const
{
    if (!m_definition.registers_ui)
        return;

    // One owner publishes the shared selector. Registering the same fragment
    // from every dialect would create duplicate controls in the printer page.
    orchestrator_add_ui_fragment(
        m_orchestrator,
        "printer_fff.ui",
        "gcode_firmware_plugin",
        printer_ui_fragment(),
        1);
}

void BuiltinGCodeFirmwarePlugin::run_impl(const plugin_run_context *run_ctx) const
{
    run_ctx_gcode_firmware *context = plugin_ctx_as_gcode_firmware(run_ctx);
    if (context == nullptr)
        return;

    // Session ownership moves to the host and ends after the selected output
    // writer completes. A fresh instance isolates every export's machine state.
    const GCodeScriptProcessorView scripts = context->script_processor != nullptr ?
        GCodeScriptProcessorView(context->script_processor) : GCodeScriptProcessorView();
    context->instance = make_gcode_firmware_instance(
        m_definition.factory(scripts, run_ctx->plugin_storage));
}

void register_firmware(orchestrator_handle *orchestrator,
                       BuiltinGCodeFirmwarePlugin &plugin)
{
    orchestrator_register_plugin(orchestrator, plugin.c_instance());
}

} // namespace

const char *printer_ui_fragment() noexcept
{
    return "page:General:printer\n"
           "group:Firmware\n"
           "\tsetting:insert$aftersetting$step_gcode_plugin:gcode_firmware_plugin\n";
}

void register_marlin1_gcode_firmware_plugin(orchestrator_handle *orchestrator)
{
    static BuiltinGCodeFirmwarePlugin plugin(orchestrator, k_marlin1_definition);
    register_firmware(orchestrator, plugin);
}

void register_marlin2_gcode_firmware_plugin(orchestrator_handle *orchestrator)
{
    static BuiltinGCodeFirmwarePlugin plugin(orchestrator, k_marlin2_definition);
    register_firmware(orchestrator, plugin);
}

void register_prusa_gcode_firmware_plugin(orchestrator_handle *orchestrator)
{
    static BuiltinGCodeFirmwarePlugin plugin(orchestrator, k_prusa_definition);
    register_firmware(orchestrator, plugin);
}

void register_reprap_gcode_firmware_plugin(orchestrator_handle *orchestrator)
{
    static BuiltinGCodeFirmwarePlugin plugin(orchestrator, k_reprap_definition);
    register_firmware(orchestrator, plugin);
}

void register_sprinter_gcode_firmware_plugin(orchestrator_handle *orchestrator)
{
    static BuiltinGCodeFirmwarePlugin plugin(orchestrator, k_sprinter_definition);
    register_firmware(orchestrator, plugin);
}

void register_klipper_gcode_firmware_plugin(orchestrator_handle *orchestrator)
{
    static BuiltinGCodeFirmwarePlugin plugin(orchestrator, k_klipper_definition);
    register_firmware(orchestrator, plugin);
}

void register_builtin_gcode_firmware_plugins(orchestrator_handle *orchestrator)
{
    // Marlin 2 has the lowest priority and therefore becomes the first enum
    // value and default when no printer preset made an explicit selection.
    register_marlin2_gcode_firmware_plugin(orchestrator);
    register_marlin1_gcode_firmware_plugin(orchestrator);
    register_prusa_gcode_firmware_plugin(orchestrator);
    register_reprap_gcode_firmware_plugin(orchestrator);
    register_sprinter_gcode_firmware_plugin(orchestrator);
    register_klipper_gcode_firmware_plugin(orchestrator);
}

}}} // namespace slic3r_api::GCodeGeneration::Firmware
