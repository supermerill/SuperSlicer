///|/ Copyright (c) SuperSlicer 2026 Durand Rémi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/
#include <algorithm>
#include <string>
#include <utility>
#include <vector>

#include <boost/log/trivial.hpp>

#include "libslic3r/Api/plugin/c/slic3r_orchestrator.h"
#include "libslic3r/Api/plugin/c/slic3r_extrusion_property.h"
#include "libslic3r/Api/plugin/c/slic3r_data_tree.h"
#include "libslic3r/Print.hpp"

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
