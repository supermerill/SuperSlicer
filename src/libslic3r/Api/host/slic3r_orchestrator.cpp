///|/ Copyright (c) SuperSlicer 2026 Durand Rémi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/
#include <algorithm>
#include <string>
#include <utility>
#include <vector>

#include <boost/log/trivial.hpp>
#include <boost/filesystem/path.hpp>

#include "libslic3r/Api/plugin/c/slic3r_orchestrator.h"
#include "libslic3r/Api/plugin/c/slic3r_extrusion_property.h"
#include "libslic3r/Api/plugin/c/slic3r_data_tree.h"
#include "libslic3r/Api/host/ApiHostUtils.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/Steps/StepPipeline.hpp"

#include "Orchestrator.hpp"
#include "Plugin.hpp"

namespace {

struct GenericPropertyInfo
{
    slic3r_property_type type;
    const char *name;
    uint32_t byte_count;
    uint32_t alignment;
};

struct BuiltinGCodeScriptTypeInfo
{
    gcode_script_type type;
    const char *name;
};

const std::vector<BuiltinGCodeScriptTypeInfo> &builtin_gcode_script_type_infos() {
    static const std::vector<BuiltinGCodeScriptTypeInfo> infos = {
        {GCODE_SCRIPT_TYPE_START_GCODE, "start_gcode"},
        {GCODE_SCRIPT_TYPE_END_GCODE, "end_gcode"},
        {GCODE_SCRIPT_TYPE_EXTRUSION_CUSTOM, "extrusion_custom_gcode_script"},
        {GCODE_SCRIPT_TYPE_START_FILAMENT_GCODE, "start_filament_gcode"},
        {GCODE_SCRIPT_TYPE_END_FILAMENT_GCODE, "end_filament_gcode"},
        {GCODE_SCRIPT_TYPE_BEFORE_LAYER_GCODE, "before_layer_gcode"},
        {GCODE_SCRIPT_TYPE_LAYER_GCODE, "layer_gcode"},
        {GCODE_SCRIPT_TYPE_TOOLCHANGE_GCODE, "toolchange_gcode"},
        {GCODE_SCRIPT_TYPE_BETWEEN_OBJECTS_GCODE, "between_objects_gcode"},
        {GCODE_SCRIPT_TYPE_FEATURE_GCODE, "feature_gcode"},
        {GCODE_SCRIPT_TYPE_COLOR_CHANGE_GCODE, "color_change_gcode"},
        {GCODE_SCRIPT_TYPE_PAUSE_PRINT_GCODE, "pause_print_gcode"},
        {GCODE_SCRIPT_TYPE_TEMPLATE_CUSTOM_GCODE, "template_custom_gcode"},
    };
    return infos;
}

const BuiltinGCodeScriptTypeInfo *builtin_gcode_script_type_info(gcode_script_type type) {
    for (const BuiltinGCodeScriptTypeInfo &info : builtin_gcode_script_type_infos())
        if (info.type == type)
            return &info;
    return nullptr;
}

const BuiltinGCodeScriptTypeInfo *builtin_gcode_script_type_info(const char *name) {
    if (name == nullptr)
        return nullptr;
    for (const BuiltinGCodeScriptTypeInfo &info : builtin_gcode_script_type_infos())
        if (std::string(info.name) == name)
            return &info;
    return nullptr;
}

const std::vector<GenericPropertyInfo> &builtin_property_infos()
{
    static const std::vector<GenericPropertyInfo> infos = {
        { SLIC3R_PROPERTY_TYPE_EXTRUSION_ATTRIBUTES,      "slic3r.extrusion.attributes",      sizeof(c_extrusion_property_attributes),      alignof(c_extrusion_property_attributes) },
        { SLIC3R_PROPERTY_TYPE_EXTRUSION_SPEED,           "slic3r.extrusion.speed",           sizeof(c_extrusion_property_speed),           alignof(c_extrusion_property_speed) },
        { SLIC3R_PROPERTY_TYPE_EXTRUSION_MODIFIER,        "slic3r.extrusion.modifier",        sizeof(c_extrusion_property_modifier),        alignof(c_extrusion_property_modifier) },
        { SLIC3R_PROPERTY_TYPE_EXTRUSION_CUSTOM_GCODE,    "slic3r.extrusion.custom_gcode",    sizeof(c_extrusion_property_custom_gcode),    alignof(c_extrusion_property_custom_gcode) },
        { SLIC3R_PROPERTY_TYPE_EXTRUSION_SPECIAL_COMMAND, "slic3r.extrusion.special_command", sizeof(c_extrusion_property_special_command), alignof(c_extrusion_property_special_command) },
        { SLIC3R_PROPERTY_TYPE_EXTRUSION_OVERHANG,        "slic3r.extrusion.overhang",        sizeof(c_extrusion_property_overhang),        alignof(c_extrusion_property_overhang) },
        { SLIC3R_PROPERTY_TYPE_EXTRUSION_Z_OFFSET,        "slic3r.extrusion.z_offset",        sizeof(c_extrusion_property_z_offset),        alignof(c_extrusion_property_z_offset) },
        { SLIC3R_PROPERTY_TYPE_EXTRUSION_PERIMETER,       "slic3r.extrusion.perimeter",       sizeof(c_extrusion_property_perimeter),       alignof(c_extrusion_property_perimeter) },
        { SLIC3R_PROPERTY_TYPE_EXTRUSION_INFILL,          "slic3r.extrusion.infill",          sizeof(c_extrusion_property_infill),          alignof(c_extrusion_property_infill) },
        { SLIC3R_PROPERTY_TYPE_EXTRUSION_AXIS,            "slic3r.extrusion.axis",            sizeof(c_extrusion_property_extrusion_axis),  alignof(c_extrusion_property_extrusion_axis) },
        { SLIC3R_PROPERTY_TYPE_LAYER_SUPPORT,             "slic3r.layer.support",             sizeof(c_layer_support_property),             alignof(c_layer_support_property) },
        { SLIC3R_PROPERTY_TYPE_LAYER_BRIM,                "slic3r.layer.brim",                sizeof(c_layer_brim_property),                alignof(c_layer_brim_property) },
        { SLIC3R_PROPERTY_TYPE_LAYER_ADHESION,            "slic3r.layer.adhesion",            sizeof(c_layer_adhesion_property),            alignof(c_layer_adhesion_property) },
    };
    return infos;
}

const GenericPropertyInfo *builtin_property_info(slic3r_property_type type)
{
    for (const GenericPropertyInfo &info : builtin_property_infos())
        if (info.type == type)
            return &info;
    return nullptr;
}

bool is_power_of_two(uint32_t value)
{
    return value != 0 && (value & (value - 1)) == 0;
}

Slic3r::Orchestrator *to_orchestrator(orchestrator_handle *orch)
{
    return orch == nullptr ? &Slic3r::Orchestrator::instance() :
                             reinterpret_cast<Slic3r::Orchestrator *>(orch);
}

const Slic3r::Orchestrator *to_orchestrator(const orchestrator_handle *orch)
{
    return orch == nullptr ? &Slic3r::Orchestrator::instance() :
                             reinterpret_cast<const Slic3r::Orchestrator *>(orch);
}

Slic3r::PluginPackageLoadErrorCode plugin_package_load_error_code(raw_plugin_package_load_error_code code)
{
    switch (code) {
    case RAW_PLUGIN_PACKAGE_LOAD_ERROR_PACKAGE_MISSING:
        return Slic3r::PluginPackageLoadErrorCode::PackageMissing;
    case RAW_PLUGIN_PACKAGE_LOAD_ERROR_LIBRARY_OPEN_FAILED:
        return Slic3r::PluginPackageLoadErrorCode::LibraryOpenFailed;
    case RAW_PLUGIN_PACKAGE_LOAD_ERROR_MISSING_ABI_EXPORT:
    case RAW_PLUGIN_PACKAGE_LOAD_ERROR_RESERVED_MISSING_API_VERSIONS_EXPORT:
        return Slic3r::PluginPackageLoadErrorCode::MissingAbiExport;
    case RAW_PLUGIN_PACKAGE_LOAD_ERROR_RESERVED_ABI_MISMATCH:
    case RAW_PLUGIN_PACKAGE_LOAD_ERROR_API_HEADER_VERSION_MISMATCH:
        return Slic3r::PluginPackageLoadErrorCode::ApiHeaderVersionMismatch;
    case RAW_PLUGIN_PACKAGE_LOAD_ERROR_MISSING_REGISTRATION_EXPORT:
        return Slic3r::PluginPackageLoadErrorCode::MissingRegistrationExport;
    case RAW_PLUGIN_PACKAGE_LOAD_ERROR_REGISTRATION_FAILED:
        return Slic3r::PluginPackageLoadErrorCode::RegistrationFailed;
    case RAW_PLUGIN_PACKAGE_LOAD_ERROR_NO_PLUGINS_REGISTERED:
        return Slic3r::PluginPackageLoadErrorCode::NoPluginsRegistered;
    case RAW_PLUGIN_PACKAGE_LOAD_ERROR_PYTHON_RUNTIME_UNAVAILABLE:
        return Slic3r::PluginPackageLoadErrorCode::PythonRuntimeUnavailable;
    case RAW_PLUGIN_PACKAGE_LOAD_ERROR_PYTHON_READ_FAILED:
        return Slic3r::PluginPackageLoadErrorCode::PythonReadFailed;
    case RAW_PLUGIN_PACKAGE_LOAD_ERROR_PYTHON_COMPILE_FAILED:
        return Slic3r::PluginPackageLoadErrorCode::PythonCompileFailed;
    case RAW_PLUGIN_PACKAGE_LOAD_ERROR_PYTHON_IMPORT_FAILED:
        return Slic3r::PluginPackageLoadErrorCode::PythonImportFailed;
    case RAW_PLUGIN_PACKAGE_LOAD_ERROR_PYTHON_REGISTRATION_FAILED:
        return Slic3r::PluginPackageLoadErrorCode::PythonRegistrationFailed;
    case RAW_PLUGIN_PACKAGE_LOAD_ERROR_CONFIGURED_PLUGIN_MISSING:
        return Slic3r::PluginPackageLoadErrorCode::ConfiguredPluginMissing;
    case RAW_PLUGIN_PACKAGE_LOAD_ERROR_INVALID_PACKAGE:
    default:
        return Slic3r::PluginPackageLoadErrorCode::InvalidPackage;
    }
}

} // namespace

extern "C" {

void orchestrator_register_plugin(orchestrator_handle *orch, plugin_instance plugin) {
    Slic3r::Orchestrator *orchestrator = orch == nullptr ? &Slic3r::Orchestrator::instance() :
                                                           reinterpret_cast<Slic3r::Orchestrator *>(orch);
    if (orchestrator != nullptr) {
        orchestrator->register_plugin(plugin);
    }
}

void orchestrator_register_plugin_from_package(orchestrator_handle *orch,
                                               plugin_instance plugin,
                                               const char *package_root)
{
    if (package_root == nullptr || package_root[0] == '\0')
        return;
    Slic3r::Orchestrator *orchestrator = to_orchestrator(orch);
    if (orchestrator == nullptr)
        return;

    Slic3r::Orchestrator::PluginRegistrationScope registration_scope(
        orchestrator->plugin_registration_scope(package_root, true));
    orchestrator->register_plugin(plugin);
}

// Keep registration failures inside the C barrier and identify the offending
// declaration in the log. No member lookup occurs until loading is complete.
int32_t orchestrator_register_activation_group(orchestrator_handle *orch,
    const char *group_id, const_strings_t members)
{
    try {
        if (orch == nullptr || group_id == nullptr || members.items == nullptr || members.size < 2)
            throw std::invalid_argument("Activation group requires an orchestrator, an ID and at least two members.");
        std::vector<std::string> ids;
        ids.reserve(members.size);
        for (uint32_t index = 0; index < members.size; ++index) {
            if (members.items[index] == nullptr)
                throw std::invalid_argument("Activation group contains a null member.");
            ids.emplace_back(members.items[index]);
        }
        std::string error;
        if (!to_orchestrator(orch)->register_activation_group(group_id, ids, error))
            throw std::invalid_argument(error);
        return 1;
    } catch (const std::exception &error) {
        BOOST_LOG_TRIVIAL(error) << "Cannot register activation group '" << (group_id ? group_id : "<null>")
                                 << "': " << error.what();
    } catch (...) {
        BOOST_LOG_TRIVIAL(error) << "Unexpected activation group registration failure.";
    }
    return 0;
}

slicing_step_t orchestrator_register_step(orchestrator_handle *orch,
                                          const char *namespaced_name,
                                          slicing_step_t invalidates_step)
{
    try {
        return to_orchestrator(orch)->register_step(namespaced_name, invalidates_step);
    } catch (...) {
        return STEP_NONE;
    }
}

const char *orchestrator_step_name(const orchestrator_handle *orch, slicing_step_t step)
{
    try {
        const Slic3r::Orchestrator::DynamicStepInfo *info = to_orchestrator(orch)->step_info(step);
        return info == nullptr ? nullptr : info->name.c_str();
    } catch (...) {
        return nullptr;
    }
}

const orchestrator_plugin_handle *orchestrator_find_plugin(orchestrator_handle *orch,
                                                           const char *plugin_id)
{
    if (plugin_id == nullptr || plugin_id[0] == '\0')
        return nullptr;
    try {
        const Slic3r::Plugin *plugin = to_orchestrator(orch)->get_plugin(plugin_id);
        return reinterpret_cast<const orchestrator_plugin_handle *>(plugin);
    } catch (...) {
        return nullptr;
    }
}

const orchestrator_plugin_handle *orchestrator_select_plugin(orchestrator_handle *orch,
                                                             const config_handle *config,
                                                             slicing_step_t step,
                                                             const char *exclusive_group)
{
    if (exclusive_group == nullptr || exclusive_group[0] == '\0')
        return nullptr;
    try {
        Slic3r::Plugin *plugin = Slic3r::Steps::selected_active_plugin_from_group(
            *to_orchestrator(orch),
            step,
            exclusive_group,
            Slic3r::ApiHost::to_config(config));
        return reinterpret_cast<const orchestrator_plugin_handle *>(plugin);
    } catch (...) {
        return nullptr;
    }
}

const char *orchestrator_plugin_id(const orchestrator_plugin_handle *plugin)
{
    const Slic3r::Plugin *host_plugin = reinterpret_cast<const Slic3r::Plugin *>(plugin);
    return host_plugin == nullptr ? nullptr : host_plugin->get_id().c_str();
}

slicing_step_t orchestrator_plugin_step(const orchestrator_plugin_handle *plugin)
{
    const Slic3r::Plugin *host_plugin = reinterpret_cast<const Slic3r::Plugin *>(plugin);
    return host_plugin == nullptr ? STEP_NONE : host_plugin->get_step();
}

raw_plugin_execution_status orchestrator_execute_plugin(
    orchestrator_handle *orch,
    const orchestrator_plugin_handle *plugin,
    const print_handle *print,
    const raw_plugin_run_payload *payloads,
    uint32_t run_count)
{
    try {
        Slic3r::Orchestrator *host_orchestrator = to_orchestrator(orch);
        Slic3r::Plugin *host_plugin = reinterpret_cast<Slic3r::Plugin *>(
            const_cast<orchestrator_plugin_handle *>(plugin));
        if (host_orchestrator == nullptr || !host_orchestrator->owns_plugin(host_plugin))
            return RAW_PLUGIN_EXECUTION_INVALID_ARGUMENT;

        Slic3r::Print *host_print = reinterpret_cast<Slic3r::Print *>(
            const_cast<print_handle *>(print));
        return host_orchestrator->execute_plugin(*host_plugin, host_print, payloads, run_count);
    } catch (...) {
        return RAW_PLUGIN_EXECUTION_PLUGIN_ERROR;
    }
}

void orchestrator_begin_plugin_package_load(orchestrator_handle *orch, const char *package_root)
{
    if (package_root == nullptr || package_root[0] == '\0')
        return;
    try {
        const boost::filesystem::path root(package_root);
        to_orchestrator(orch)->begin_plugin_package_load(root.filename().string(), root.string());
    } catch (...) {
    }
}

void orchestrator_report_plugin_package_load_error(orchestrator_handle *orch,
                                                   const char *package_root,
                                                   raw_plugin_package_load_error_code code,
                                                   const char *detail)
{
    if (package_root == nullptr || package_root[0] == '\0')
        return;
    try {
        const boost::filesystem::path root(package_root);
        Slic3r::PluginPackageLoadIssue issue;
        issue.code = plugin_package_load_error_code(code);
        if (detail != nullptr)
            issue.detail = detail;
        to_orchestrator(orch)->report_plugin_package_load_issue(root.filename().string(), std::move(issue));
    } catch (...) {
    }
}

void orchestrator_finish_plugin_package_load(orchestrator_handle *orch, const char *package_root)
{
    if (package_root == nullptr || package_root[0] == '\0')
        return;
    try {
        const boost::filesystem::path root(package_root);
        to_orchestrator(orch)->finish_plugin_package_load(root.filename().string());
    } catch (...) {
    }
}

int32_t orchestrator_register_translation_catalog(orchestrator_handle *orch,
                                                  const char *domain,
                                                  const char *locale_directory)
{
    try {
        Slic3r::Orchestrator *orchestrator = to_orchestrator(orch);
        return orchestrator == nullptr ? -1 :
                                       orchestrator->register_translation_catalog(domain, locale_directory);
    } catch (...) {
        return -3;
    }
}

gcode_script_type gcode_script_register_type(orchestrator_handle *orch, const char *namespaced_name) {
    if (namespaced_name == nullptr || namespaced_name[0] == '\0')
        return GCODE_SCRIPT_TYPE_INVALID;
    if (const BuiltinGCodeScriptTypeInfo *info = builtin_gcode_script_type_info(namespaced_name))
        return info->type;

    Slic3r::Orchestrator *orchestrator = to_orchestrator(orch);
    return orchestrator == nullptr ? GCODE_SCRIPT_TYPE_INVALID :
                                     orchestrator->register_gcode_script_type(namespaced_name);
}

const char *gcode_script_type_name(const orchestrator_handle *orch, gcode_script_type type) {
    if (const BuiltinGCodeScriptTypeInfo *info = builtin_gcode_script_type_info(type))
        return info->name;
    const Slic3r::Orchestrator *orchestrator = to_orchestrator(orch);
    const Slic3r::Orchestrator::GCodeScriptTypeInfo *info = orchestrator == nullptr ?
        nullptr :
        orchestrator->gcode_script_type_info(type);
    return info == nullptr ? nullptr : info->name.c_str();
}

bridge_detector_instance orchestrator_create_bridge_detector(orchestrator_handle *orch,
                                                             const bridge_detector_create_input *input) {
    bridge_detector_instance out = {};
    if (input == nullptr)
        return out;
    Slic3r::Orchestrator *orchestrator = orch == nullptr ? &Slic3r::Orchestrator::instance() :
                                                           reinterpret_cast<Slic3r::Orchestrator *>(orch);
    return orchestrator == nullptr ? out : orchestrator->create_bridge_detector(*input);
}

slic3r_property_type orchestrator_register_property(orchestrator_handle *orch,
                                                    const char *namespaced_name,
                                                    uint32_t byte_count,
                                                    uint32_t alignment)
{
    if (namespaced_name == nullptr || namespaced_name[0] == '\0' ||
        byte_count == 0 || !is_power_of_two(alignment))
        return SLIC3R_PROPERTY_TYPE_INVALID;

    /*
    Built-in names are reserved. A plugin may use the numeric built-in id
    directly, but it must not re-register the same name with a different ABI
    contract.
    */
    for (const GenericPropertyInfo &info : builtin_property_infos())
        if (std::string(info.name) == namespaced_name)
            return info.byte_count == byte_count && info.alignment == alignment ?
                info.type :
                SLIC3R_PROPERTY_TYPE_INVALID;

    Slic3r::Orchestrator *orchestrator = to_orchestrator(orch);
    return orchestrator == nullptr ?
        SLIC3R_PROPERTY_TYPE_INVALID :
        orchestrator->register_property(namespaced_name, byte_count, alignment);
}

uint32_t orchestrator_property_byte_count(const orchestrator_handle *orch, slic3r_property_type type)
{
    if (const GenericPropertyInfo *info = builtin_property_info(type))
        return info->byte_count;
    const Slic3r::Orchestrator *orchestrator = to_orchestrator(orch);
    const Slic3r::Orchestrator::PropertyInfo *info =
        orchestrator == nullptr ? nullptr : orchestrator->property_info(type);
    return info == nullptr ? 0u : info->byte_count;
}

uint32_t orchestrator_property_alignment(const orchestrator_handle *orch, slic3r_property_type type)
{
    if (const GenericPropertyInfo *info = builtin_property_info(type))
        return info->alignment;
    const Slic3r::Orchestrator *orchestrator = to_orchestrator(orch);
    const Slic3r::Orchestrator::PropertyInfo *info =
        orchestrator == nullptr ? nullptr : orchestrator->property_info(type);
    return info == nullptr ? 0u : info->alignment;
}

const char *orchestrator_property_name(const orchestrator_handle *orch, slic3r_property_type type)
{
    if (const GenericPropertyInfo *info = builtin_property_info(type))
        return info->name;
    const Slic3r::Orchestrator *orchestrator = to_orchestrator(orch);
    const Slic3r::Orchestrator::PropertyInfo *info =
        orchestrator == nullptr ? nullptr : orchestrator->property_info(type);
    return info == nullptr ? nullptr : info->name.c_str();
}

int32_t orchestrator_register_generic_facets_annotation(
    orchestrator_handle *orch,
    const raw_generic_facets_annotation_def *def)
{
    if (def == nullptr || def->key == nullptr || def->label == nullptr ||
        def->enforce_label == nullptr || def->block_label == nullptr ||
        def->icon_svg == nullptr || def->icon_svg[0] == '\0')
        return -1;

    try {
        Slic3r::Orchestrator *orchestrator = orch == nullptr ? &Slic3r::Orchestrator::instance() :
                                                               reinterpret_cast<Slic3r::Orchestrator *>(orch);
        if (orchestrator == nullptr)
            return -1;

        Slic3r::GenericFacetsAnnotationDefinition native_def;
        native_def.key = def->key;
        native_def.label = def->label;
        native_def.enforce_label = def->enforce_label;
        native_def.block_label = def->block_label;
        if (def->translation_domain != nullptr)
            native_def.translation_domain = def->translation_domain;
        native_def.icon_svg = def->icon_svg;
        return orchestrator->register_generic_facets_annotation(std::move(native_def)) ? 1 : -2;
    } catch (...) {
        return -3;
    }
}

int32_t orchestrator_add_ui_fragment(orchestrator_handle *orch,
                                     const char *target_file,
                                     const char *fragment_id,
                                     const char *ui_fragment,
                                     int32_t priority)
{
    if (target_file == nullptr || fragment_id == nullptr || ui_fragment == nullptr)
        return -1;

    try {
        Slic3r::Orchestrator *orchestrator = orch == nullptr ? &Slic3r::Orchestrator::instance() :
                                                               reinterpret_cast<Slic3r::Orchestrator *>(orch);
        if (orchestrator == nullptr)
            return -1;
        return orchestrator->add_ui_fragment(target_file, fragment_id, ui_fragment, priority) ? 1 : 0;
    } catch (...) {
        return -2;
    }
}

int32_t orchestrator_add_gui_rule(orchestrator_handle *orch, const raw_gui_rule *rule)
{
    if (rule == nullptr)
        return -1;

    try {
        Slic3r::Orchestrator *orchestrator = orch == nullptr ? &Slic3r::Orchestrator::instance() :
                                                               reinterpret_cast<Slic3r::Orchestrator *>(orch);
        if (orchestrator == nullptr)
            return -1;
        return orchestrator->add_gui_rule(rule) ? 1 : 0;
    } catch (...) {
        return -2;
    }
}

int orchestrator_plugin_is_cancelled(plugin_host_context *host_context)
{
    return host_context != nullptr && host_context->orchestrator != nullptr &&
                   host_context->orchestrator->is_plugin_cancelled() ?
               1 :
               0;
}

void orchestrator_plugin_report_warning(plugin_host_context *host_context, const char *message)
{
    const char *plugin_id = host_context != nullptr && host_context->plugin != nullptr ?
                                host_context->plugin->get_id().c_str() :
                                "<unknown>";
    BOOST_LOG_TRIVIAL(warning) << "Plugin warning from " << plugin_id << ": " << (message != nullptr ? message : "");

    if (host_context != nullptr && host_context->orchestrator != nullptr)
        host_context->orchestrator->add_plugin_message(Slic3r::Orchestrator::PluginMessageLevel::Warning,
                                                       host_context->plugin,
                                                       host_context->step,
                                                       message);
}

void orchestrator_plugin_report_error(plugin_host_context *host_context, const char *message)
{
    const char *plugin_id = host_context != nullptr && host_context->plugin != nullptr ?
                                host_context->plugin->get_id().c_str() :
                                "<unknown>";
    BOOST_LOG_TRIVIAL(error) << "Plugin error from " << plugin_id << ": " << (message != nullptr ? message : "");

    if (host_context != nullptr && host_context->execution_error != nullptr)
        host_context->execution_error->store(true, std::memory_order_relaxed);

    if (host_context != nullptr && host_context->orchestrator != nullptr) {
        host_context->orchestrator->add_plugin_message(Slic3r::Orchestrator::PluginMessageLevel::Error,
                                                       host_context->plugin,
                                                       host_context->step,
                                                       message);
        host_context->orchestrator->request_plugin_cancel();
    }
}

void orchestrator_plugin_report_progress(plugin_host_context *host_context, double progress, const char *message)
{
    if (host_context == nullptr || host_context->print == nullptr)
        return;

    const int percent = int(std::clamp(progress, 0.0, 1.0) * 100.0 + 0.5);

    if (message != nullptr && message[0] != '\0') {
        host_context->print->set_status(percent,
                                        message,
                                        Slic3r::PrintBase::SlicingStatus::SECONDARY_STATE);
        return;
    }

    const std::string plugin_id = host_context->plugin != nullptr ? host_context->plugin->get_id() : "<unknown>";
    const std::string message_text = "Plugin " + plugin_id;
    host_context->print->set_status(percent,
                                    message_text,
                                    Slic3r::PrintBase::SlicingStatus::SECONDARY_STATE);
}

}
