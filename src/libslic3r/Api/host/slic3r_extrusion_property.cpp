///|/ Copyright (c) SuperSlicer 2026 Durand Rémi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/

#include <cstddef>
#include <cstdint>
#include <cmath>
#include <cstring>
#include <limits>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "libslic3r/Api/internal/ExtrusionPropertyAccess.hpp"
#include "libslic3r/Api/plugin/c/slic3r_extrusion_property.h"
#include "libslic3r/ExtrusionEntity.hpp"

#include "Orchestrator.hpp"

/*
Stored G-code script arguments
==============================

Script producers pass ordinary C values. This file validates them and packs a
self-describing immutable blob into the extrusion entity. The blob contains no
pointers, so the normal extrusion clone/move machinery can preserve it without
knowing anything about Config or PlaceholderParser.
*/

struct raw_gcode_script_arguments
{
    uint32_t magic;
    uint32_t byte_size;
    uint32_t count;
    uint32_t reserved;
};

namespace Slic3r {

static constexpr uint32_t gcode_script_arguments_magic = 0x47534131u;

struct StoredGCodeScriptArgument
{
    uint32_t type;
    uint32_t key_offset;
    uint32_t key_size;
    uint32_t value_offset;
    uint32_t value_count;
    int32_t integer_value;
    double float_value;
};

static bool checked_blob_size(size_t value, uint32_t *out);
static bool append_blob_bytes(std::vector<uint8_t> &blob,
                              const void *data,
                              size_t byte_count,
                              size_t alignment,
                              uint32_t *offset_out);
static bool valid_stored_arguments(const raw_gcode_script_arguments *arguments,
                                   uint32_t available_bytes);
static raw_gcode_script_arguments_status build_stored_arguments(
    const raw_gcode_script_argument *arguments,
    uint32_t argument_count,
    std::vector<uint8_t> &blob);

// Convert a checked platform size to the compact offsets stored in the ABI blob.
static bool checked_blob_size(size_t value, uint32_t *out)
{
    if (out == nullptr || value > std::numeric_limits<uint32_t>::max())
        return false;
    *out = static_cast<uint32_t>(value);
    return true;
}

// Append one payload with enough padding for direct typed reads from the blob.
static bool append_blob_bytes(std::vector<uint8_t> &blob,
                              const void *data,
                              size_t byte_count,
                              size_t alignment,
                              uint32_t *offset_out)
{
    if (offset_out == nullptr || alignment == 0 || (byte_count > 0 && data == nullptr))
        return false;
    const size_t remainder = blob.size() % alignment;
    const size_t padding = remainder == 0 ? 0 : alignment - remainder;
    if (blob.size() > std::numeric_limits<uint32_t>::max() - padding ||
        blob.size() + padding > std::numeric_limits<uint32_t>::max() - byte_count)
        return false;
    blob.insert(blob.end(), padding, uint8_t(0));
    *offset_out = static_cast<uint32_t>(blob.size());
    if (byte_count > 0) {
        const uint8_t *bytes = static_cast<const uint8_t *>(data);
        blob.insert(blob.end(), bytes, bytes + byte_count);
    }
    return true;
}

// Validate every offset before an opaque blob is exposed to a plugin callback.
static bool valid_stored_arguments(const raw_gcode_script_arguments *arguments,
                                   uint32_t available_bytes)
{
    if (arguments == nullptr || available_bytes < sizeof(raw_gcode_script_arguments) ||
        arguments->magic != gcode_script_arguments_magic || arguments->byte_size != available_bytes)
        return false;
    if (arguments->count >
        (available_bytes - sizeof(raw_gcode_script_arguments)) / sizeof(StoredGCodeScriptArgument))
        return false;
    const size_t entries_end = sizeof(raw_gcode_script_arguments) +
        size_t(arguments->count) * sizeof(StoredGCodeScriptArgument);
    const StoredGCodeScriptArgument *entries = reinterpret_cast<const StoredGCodeScriptArgument *>(
        reinterpret_cast<const uint8_t *>(arguments) + sizeof(raw_gcode_script_arguments));
    for (uint32_t index = 0; index < arguments->count; ++index) {
        const StoredGCodeScriptArgument &entry = entries[index];
        if (entry.type > RAW_GCODE_SCRIPT_ARGUMENT_FLOATS || entry.key_size == 0 ||
            entry.key_offset < entries_end || entry.key_offset > available_bytes ||
            entry.key_size > available_bytes - entry.key_offset)
            return false;
        const char *key = reinterpret_cast<const char *>(arguments) + entry.key_offset;
        if (key[entry.key_size - 1] != '\0' ||
            std::memchr(key, '\0', entry.key_size - 1) != nullptr)
            return false;

        // Scalar values live directly in the entry. Validate them too so a
        // corrupted blob cannot introduce values the public setter rejects.
        if (entry.type == RAW_GCODE_SCRIPT_ARGUMENT_BOOL &&
            entry.integer_value != 0 && entry.integer_value != 1)
            return false;
        if (entry.type == RAW_GCODE_SCRIPT_ARGUMENT_FLOAT && !std::isfinite(entry.float_value))
            return false;

        size_t value_bytes = 0;
        if (entry.type == RAW_GCODE_SCRIPT_ARGUMENT_STRING)
            value_bytes = entry.value_count;
        else if (entry.type == RAW_GCODE_SCRIPT_ARGUMENT_INTS) {
            if (entry.value_count > available_bytes / sizeof(int32_t) ||
                entry.value_offset % alignof(int32_t) != 0)
                return false;
            value_bytes = size_t(entry.value_count) * sizeof(int32_t);
        } else if (entry.type == RAW_GCODE_SCRIPT_ARGUMENT_FLOATS) {
            if (entry.value_count > available_bytes / sizeof(double) ||
                entry.value_offset % alignof(double) != 0)
                return false;
            value_bytes = size_t(entry.value_count) * sizeof(double);
        }
        if (value_bytes > 0 &&
            (entry.value_offset < entries_end || entry.value_offset > available_bytes ||
             value_bytes > available_bytes - entry.value_offset))
            return false;
        if (entry.type == RAW_GCODE_SCRIPT_ARGUMENT_FLOATS) {
            const double *values = reinterpret_cast<const double *>(
                reinterpret_cast<const uint8_t *>(arguments) + entry.value_offset);
            for (uint32_t value_idx = 0; value_idx < entry.value_count; ++value_idx)
                if (!std::isfinite(values[value_idx]))
                    return false;
        }
    }
    return true;
}

// Validate the complete batch, then build the pointer-free representation used by extrusion storage.
static raw_gcode_script_arguments_status build_stored_arguments(
    const raw_gcode_script_argument *arguments,
    uint32_t argument_count,
    std::vector<uint8_t> &blob)
{
    if (argument_count > 0 && arguments == nullptr)
        return RAW_GCODE_SCRIPT_ARGUMENTS_INVALID_ARGUMENT;
    const size_t fixed_size = sizeof(raw_gcode_script_arguments) +
        size_t(argument_count) * sizeof(StoredGCodeScriptArgument);
    uint32_t ignored_fixed_size = 0;
    if (!checked_blob_size(fixed_size, &ignored_fixed_size))
        return RAW_GCODE_SCRIPT_ARGUMENTS_INVALID_VALUE;
    blob.assign(fixed_size, uint8_t(0));
    std::vector<StoredGCodeScriptArgument> entries(argument_count);
    std::set<std::string> keys;

    for (uint32_t index = 0; index < argument_count; ++index) {
        const raw_gcode_script_argument &source = arguments[index];
        if (source.key == nullptr || source.key[0] == '\0')
            return RAW_GCODE_SCRIPT_ARGUMENTS_INVALID_KEY;
        const std::string key(source.key);
        if (!keys.insert(key).second)
            return RAW_GCODE_SCRIPT_ARGUMENTS_DUPLICATE_KEY;
        if (source.type < RAW_GCODE_SCRIPT_ARGUMENT_BOOL ||
            source.type > RAW_GCODE_SCRIPT_ARGUMENT_FLOATS)
            return RAW_GCODE_SCRIPT_ARGUMENTS_INVALID_VALUE;

        StoredGCodeScriptArgument &entry = entries[index];
        entry.type = uint32_t(source.type);
        if (!checked_blob_size(key.size() + 1, &entry.key_size))
            return RAW_GCODE_SCRIPT_ARGUMENTS_INVALID_VALUE;
        if (!append_blob_bytes(blob, key.c_str(), key.size() + 1, alignof(char), &entry.key_offset))
            return RAW_GCODE_SCRIPT_ARGUMENTS_INVALID_VALUE;

        switch (source.type) {
        case RAW_GCODE_SCRIPT_ARGUMENT_BOOL:
            if (source.value.boolean != 0 && source.value.boolean != 1)
                return RAW_GCODE_SCRIPT_ARGUMENTS_INVALID_VALUE;
            entry.integer_value = source.value.boolean;
            break;
        case RAW_GCODE_SCRIPT_ARGUMENT_INT:
            entry.integer_value = source.value.integer;
            break;
        case RAW_GCODE_SCRIPT_ARGUMENT_FLOAT:
            if (!std::isfinite(source.value.floating))
                return RAW_GCODE_SCRIPT_ARGUMENTS_INVALID_VALUE;
            entry.float_value = source.value.floating;
            break;
        case RAW_GCODE_SCRIPT_ARGUMENT_STRING:
            if (source.value.string.size > 0 && source.value.string.data == nullptr)
                return RAW_GCODE_SCRIPT_ARGUMENTS_INVALID_VALUE;
            entry.value_count = source.value.string.size;
            if (!append_blob_bytes(blob, source.value.string.data, source.value.string.size,
                                   alignof(char), &entry.value_offset))
                return RAW_GCODE_SCRIPT_ARGUMENTS_INVALID_VALUE;
            break;
        case RAW_GCODE_SCRIPT_ARGUMENT_INTS:
            if (source.value.integers.size > 0 && source.value.integers.data == nullptr)
                return RAW_GCODE_SCRIPT_ARGUMENTS_INVALID_VALUE;
            entry.value_count = source.value.integers.size;
            if (!append_blob_bytes(blob, source.value.integers.data,
                                   size_t(source.value.integers.size) * sizeof(int32_t),
                                   alignof(int32_t), &entry.value_offset))
                return RAW_GCODE_SCRIPT_ARGUMENTS_INVALID_VALUE;
            break;
        case RAW_GCODE_SCRIPT_ARGUMENT_FLOATS:
            if (source.value.floats.size > 0 && source.value.floats.data == nullptr)
                return RAW_GCODE_SCRIPT_ARGUMENTS_INVALID_VALUE;
            for (uint32_t value_idx = 0; value_idx < source.value.floats.size; ++value_idx)
                if (!std::isfinite(source.value.floats.data[value_idx]))
                    return RAW_GCODE_SCRIPT_ARGUMENTS_INVALID_VALUE;
            entry.value_count = source.value.floats.size;
            if (!append_blob_bytes(blob, source.value.floats.data,
                                   size_t(source.value.floats.size) * sizeof(double),
                                   alignof(double), &entry.value_offset))
                return RAW_GCODE_SCRIPT_ARGUMENTS_INVALID_VALUE;
            break;
        }
    }

    uint32_t total_size = 0;
    if (!checked_blob_size(blob.size(), &total_size))
        return RAW_GCODE_SCRIPT_ARGUMENTS_INVALID_VALUE;
    raw_gcode_script_arguments header = {gcode_script_arguments_magic, total_size, argument_count, 0};
    std::memcpy(blob.data(), &header, sizeof(header));
    if (!entries.empty())
        std::memcpy(blob.data() + sizeof(header), entries.data(), entries.size() * sizeof(entries.front()));
    return RAW_GCODE_SCRIPT_ARGUMENTS_SUCCESS;
}

struct PropertyInfo
{
    extrusion_property_type type;
    const char *name;
    uint32_t byte_count;
    uint32_t alignment;
};

static ExtrusionEntity *to_extrusion(extrusion_entity_handle *me)
{
    return reinterpret_cast<ExtrusionEntity *>(me);
}

static const ExtrusionEntity *to_extrusion(const extrusion_entity_handle *me)
{
    return reinterpret_cast<const ExtrusionEntity *>(me);
}

static bool is_power_of_two(uint32_t value)
{
    return value != 0 && (value & (value - 1)) == 0;
}

static Orchestrator *to_orchestrator(orchestrator_handle *me)
{
    return me == nullptr ? nullptr : reinterpret_cast<Orchestrator *>(me);
}

static const Orchestrator *to_orchestrator(const orchestrator_handle *me)
{
    return me == nullptr ? nullptr : reinterpret_cast<const Orchestrator *>(me);
}

static const std::vector<PropertyInfo> &builtin_property_infos()
{
    static const std::vector<PropertyInfo> infos = {
        { EXTRUSION_PROPERTY_TYPE_ATTRIBUTES,      "slic3r.extrusion.attributes",      sizeof(c_extrusion_property_attributes),      alignof(c_extrusion_property_attributes) },
        { EXTRUSION_PROPERTY_TYPE_SPEED,           "slic3r.extrusion.speed",           sizeof(c_extrusion_property_speed),           alignof(c_extrusion_property_speed) },
        { EXTRUSION_PROPERTY_TYPE_MODIFIER,        "slic3r.extrusion.modifier",        sizeof(c_extrusion_property_modifier),        alignof(c_extrusion_property_modifier) },
        { EXTRUSION_PROPERTY_TYPE_CUSTOM_GCODE,    "slic3r.extrusion.custom_gcode",    sizeof(c_extrusion_property_custom_gcode),    alignof(c_extrusion_property_custom_gcode) },
        { EXTRUSION_PROPERTY_TYPE_SPECIAL_COMMAND, "slic3r.extrusion.special_command", sizeof(c_extrusion_property_special_command), alignof(c_extrusion_property_special_command) },
        { EXTRUSION_PROPERTY_TYPE_OVERHANG,        "slic3r.extrusion.overhang",        sizeof(c_extrusion_property_overhang),        alignof(c_extrusion_property_overhang) },
        { EXTRUSION_PROPERTY_TYPE_Z_OFFSET,        "slic3r.extrusion.z_offset",        sizeof(c_extrusion_property_z_offset),        alignof(c_extrusion_property_z_offset) },
        { EXTRUSION_PROPERTY_TYPE_PERIMETER,       "slic3r.extrusion.perimeter",       sizeof(c_extrusion_property_perimeter),       alignof(c_extrusion_property_perimeter) },
        { EXTRUSION_PROPERTY_TYPE_INFILL,          "slic3r.extrusion.infill",          sizeof(c_extrusion_property_infill),          alignof(c_extrusion_property_infill) },
    };
    return infos;
}

static const PropertyInfo *builtin_property_info(extrusion_property_type type)
{
    const std::vector<PropertyInfo> &infos = builtin_property_infos();
    for (const PropertyInfo &info : infos)
        if (info.type == type)
            return &info;
    return nullptr;
}

static const Orchestrator::CustomExtrusionPropertyInfo*
custom_property_info(const Orchestrator *orchestrator, extrusion_property_type type)
{
    return orchestrator == nullptr ? nullptr : orchestrator->custom_extrusion_property_info(type);
}

static const Orchestrator::CustomExtrusionPropertyInfo*
custom_property_info(const Orchestrator *orchestrator, const char *name)
{
    return orchestrator == nullptr ? nullptr : orchestrator->custom_extrusion_property_info(name);
}

static uint32_t property_byte_count(const Orchestrator *orchestrator, extrusion_property_type type)
{
    if (const PropertyInfo *info = builtin_property_info(type))
        return info->byte_count;
    if (const Orchestrator::CustomExtrusionPropertyInfo *info = custom_property_info(orchestrator, type))
        return info->byte_count;
    return 0;
}

static uint32_t property_alignment(const Orchestrator *orchestrator, extrusion_property_type type)
{
    if (const PropertyInfo *info = builtin_property_info(type))
        return info->alignment;
    if (const Orchestrator::CustomExtrusionPropertyInfo *info = custom_property_info(orchestrator, type))
        return info->alignment;
    return 0;
}

} // namespace Slic3r

extern "C" {

extrusion_property_type extrusion_property_register_type(orchestrator_handle *orch,
                                                         const char *namespaced_name,
                                                         uint32_t byte_count,
                                                         uint32_t alignment)
{
    if (namespaced_name == nullptr || namespaced_name[0] == '\0' ||
        byte_count == 0 || !Slic3r::is_power_of_two(alignment))
        return EXTRUSION_PROPERTY_TYPE_INVALID;

    const std::vector<Slic3r::PropertyInfo> &builtin_infos = Slic3r::builtin_property_infos();
    for (const Slic3r::PropertyInfo &info : builtin_infos)
        if (std::string(info.name) == namespaced_name)
            return EXTRUSION_PROPERTY_TYPE_INVALID;

    Slic3r::Orchestrator *orchestrator = Slic3r::to_orchestrator(orch);
    return orchestrator == nullptr ?
        EXTRUSION_PROPERTY_TYPE_INVALID :
        orchestrator->register_custom_extrusion_property(namespaced_name, byte_count, alignment);
}

uint32_t extrusion_property_byte_count(const orchestrator_handle *orch, extrusion_property_type type)
{
    return Slic3r::property_byte_count(Slic3r::to_orchestrator(orch), type);
}

uint32_t extrusion_property_alignment(const orchestrator_handle *orch, extrusion_property_type type)
{
    return Slic3r::property_alignment(Slic3r::to_orchestrator(orch), type);
}

const char *extrusion_property_name(const orchestrator_handle *orch, extrusion_property_type type)
{
    if (const Slic3r::PropertyInfo *info = Slic3r::builtin_property_info(type))
        return info->name;
    if (const Slic3r::Orchestrator::CustomExtrusionPropertyInfo *info =
            Slic3r::custom_property_info(Slic3r::to_orchestrator(orch), type))
        return info->name.c_str();
    return nullptr;
}

uint32_t extrusion_property_count(const extrusion_entity_handle *entity)
{
    if (entity == nullptr)
        return 0;
    return static_cast<uint32_t>(
        Slic3r::ApiInternal::ExtrusionPropertyAccess::property_count(*Slic3r::to_extrusion(entity)));
}

extrusion_property_type extrusion_property_type_at(const extrusion_entity_handle *entity, uint32_t idx)
{
    if (entity == nullptr)
        return EXTRUSION_PROPERTY_TYPE_INVALID;
    return Slic3r::ApiInternal::ExtrusionPropertyAccess::property_type_at(*Slic3r::to_extrusion(entity), idx);
}

int32_t extrusion_property_has(const extrusion_entity_handle *entity, extrusion_property_type type)
{
    return entity != nullptr &&
        Slic3r::ApiInternal::ExtrusionPropertyAccess::has_property(*Slic3r::to_extrusion(entity), type);
}

const void *extrusion_property_data(const extrusion_entity_handle *entity, extrusion_property_type type)
{
    if (entity == nullptr)
        return nullptr;
    return Slic3r::ApiInternal::ExtrusionPropertyAccess::property_data(*Slic3r::to_extrusion(entity), type);
}

void *extrusion_property_data_mutable(extrusion_entity_handle *entity, extrusion_property_type type)
{
    if (entity == nullptr)
        return nullptr;
    return Slic3r::ApiInternal::ExtrusionPropertyAccess::property_data_mutable(*Slic3r::to_extrusion(entity), type);
}

void *extrusion_property_get_or_add_data_mutable(orchestrator_handle *orch,
                                                 extrusion_entity_handle *entity,
                                                 extrusion_property_type type)
{
    if (entity == nullptr)
        return nullptr;

    const Slic3r::Orchestrator *orchestrator = Slic3r::to_orchestrator(orch);
    const uint32_t byte_count = Slic3r::property_byte_count(orchestrator, type);
    const uint32_t alignment = Slic3r::property_alignment(orchestrator, type);
    if (byte_count == 0 || alignment == 0)
        return nullptr;
    return Slic3r::ApiInternal::ExtrusionPropertyAccess::get_or_add_property_data_mutable(
        *Slic3r::to_extrusion(entity), type, byte_count, alignment);
}

int32_t extrusion_property_remove(extrusion_entity_handle *entity, extrusion_property_type type)
{
    return entity != nullptr &&
        Slic3r::ApiInternal::ExtrusionPropertyAccess::remove_property(*Slic3r::to_extrusion(entity), type);
}

extrusion_data_id extrusion_store_data_aligned(extrusion_entity_handle *entity,
                                               const void *data,
                                               uint32_t byte_size,
                                               uint32_t alignment)
{
    if (entity == nullptr || !Slic3r::is_power_of_two(alignment) || (byte_size > 0 && data == nullptr))
        return EXTRUSION_DATA_ID_INVALID;
    return Slic3r::ApiInternal::ExtrusionPropertyAccess::store_data_aligned(
        *Slic3r::to_extrusion(entity), data, byte_size, alignment);
}

extrusion_data_id extrusion_property_store_data_aligned(extrusion_entity_handle *entity,
                                                        extrusion_property_type owner_type,
                                                        extrusion_data_id *field,
                                                        const void *data,
                                                        uint32_t byte_size,
                                                        uint32_t alignment)
{
    if (entity == nullptr || owner_type == EXTRUSION_PROPERTY_TYPE_INVALID ||
        field == nullptr || !Slic3r::is_power_of_two(alignment) || (byte_size > 0 && data == nullptr))
        return EXTRUSION_DATA_ID_INVALID;
    return Slic3r::ApiInternal::ExtrusionPropertyAccess::store_property_data_aligned(
        *Slic3r::to_extrusion(entity), owner_type, field, data, byte_size, alignment);
}

const void *extrusion_data(const extrusion_entity_handle *entity,
                           extrusion_data_id data_id,
                           uint32_t *byte_size_out)
{
    if (byte_size_out != nullptr)
        *byte_size_out = 0;
    if (entity == nullptr || data_id == EXTRUSION_DATA_ID_INVALID)
        return nullptr;
    return Slic3r::ApiInternal::ExtrusionPropertyAccess::stored_data(
        *Slic3r::to_extrusion(entity), data_id, byte_size_out);
}

int32_t extrusion_free_data(extrusion_entity_handle *entity, extrusion_data_id data_id)
{
    return entity != nullptr && data_id != EXTRUSION_DATA_ID_INVALID &&
        Slic3r::ApiInternal::ExtrusionPropertyAccess::free_data(*Slic3r::to_extrusion(entity), data_id);
}

raw_gcode_script_arguments_status extrusion_custom_gcode_set_arguments(
    extrusion_entity_handle *entity,
    const raw_gcode_script_argument *arguments,
    uint32_t argument_count)
{
    if (entity == nullptr)
        return RAW_GCODE_SCRIPT_ARGUMENTS_INVALID_ARGUMENT;
    c_extrusion_property_custom_gcode *property = static_cast<c_extrusion_property_custom_gcode *>(
        extrusion_property_data_mutable(entity, EXTRUSION_PROPERTY_TYPE_CUSTOM_GCODE));
    if (property == nullptr || property->kind != C_EXTRUSION_CUSTOM_GCODE_SCRIPT)
        return RAW_GCODE_SCRIPT_ARGUMENTS_INVALID_ARGUMENT;

    try {
        std::vector<uint8_t> blob;
        const raw_gcode_script_arguments_status status =
            Slic3r::build_stored_arguments(arguments, argument_count, blob);
        if (status != RAW_GCODE_SCRIPT_ARGUMENTS_SUCCESS)
            return status;

        // An empty context needs no resource. Clear the old owned buffer only
        // after the empty replacement has been fully validated.
        if (argument_count == 0) {
            if (property->arguments_id != EXTRUSION_DATA_ID_INVALID)
                extrusion_free_data(entity, property->arguments_id);
            property->arguments_id = EXTRUSION_DATA_ID_INVALID;
            return RAW_GCODE_SCRIPT_ARGUMENTS_SUCCESS;
        }

        const extrusion_data_id stored = extrusion_property_store_data_aligned(
            entity, EXTRUSION_PROPERTY_TYPE_CUSTOM_GCODE, &property->arguments_id,
            blob.data(), static_cast<uint32_t>(blob.size()), alignof(std::max_align_t));
        return stored != EXTRUSION_DATA_ID_INVALID ?
            RAW_GCODE_SCRIPT_ARGUMENTS_SUCCESS : RAW_GCODE_SCRIPT_ARGUMENTS_STORAGE_ERROR;
    } catch (...) {
        return RAW_GCODE_SCRIPT_ARGUMENTS_STORAGE_ERROR;
    }
}

const raw_gcode_script_arguments *extrusion_custom_gcode_arguments(
    const extrusion_entity_handle *entity,
    extrusion_data_id arguments_id)
{
    uint32_t byte_size = 0;
    const void *data = extrusion_data(entity, arguments_id, &byte_size);
    const raw_gcode_script_arguments *arguments =
        static_cast<const raw_gcode_script_arguments *>(data);
    return Slic3r::valid_stored_arguments(arguments, byte_size) ? arguments : nullptr;
}

uint32_t gcode_script_arguments_count(const raw_gcode_script_arguments *arguments)
{
    return arguments != nullptr &&
        Slic3r::valid_stored_arguments(arguments, arguments->byte_size) ? arguments->count : 0;
}

int32_t gcode_script_arguments_get(const raw_gcode_script_arguments *arguments,
                                   uint32_t index,
                                   raw_gcode_script_argument *argument_out)
{
    if (arguments == nullptr || argument_out == nullptr ||
        !Slic3r::valid_stored_arguments(arguments, arguments->byte_size) || index >= arguments->count)
        return 0;
    const Slic3r::StoredGCodeScriptArgument *entries =
        reinterpret_cast<const Slic3r::StoredGCodeScriptArgument *>(
            reinterpret_cast<const uint8_t *>(arguments) + sizeof(raw_gcode_script_arguments));
    const Slic3r::StoredGCodeScriptArgument &entry = entries[index];
    raw_gcode_script_argument out = {};
    out.key = reinterpret_cast<const char *>(arguments) + entry.key_offset;
    out.type = raw_gcode_script_argument_type(entry.type);
    switch (out.type) {
    case RAW_GCODE_SCRIPT_ARGUMENT_BOOL: out.value.boolean = entry.integer_value; break;
    case RAW_GCODE_SCRIPT_ARGUMENT_INT: out.value.integer = entry.integer_value; break;
    case RAW_GCODE_SCRIPT_ARGUMENT_FLOAT: out.value.floating = entry.float_value; break;
    case RAW_GCODE_SCRIPT_ARGUMENT_STRING:
        out.value.string.data = reinterpret_cast<const char *>(arguments) + entry.value_offset;
        out.value.string.size = entry.value_count;
        break;
    case RAW_GCODE_SCRIPT_ARGUMENT_INTS:
        out.value.integers.data = reinterpret_cast<const int32_t *>(
            reinterpret_cast<const uint8_t *>(arguments) + entry.value_offset);
        out.value.integers.size = entry.value_count;
        break;
    case RAW_GCODE_SCRIPT_ARGUMENT_FLOATS:
        out.value.floats.data = reinterpret_cast<const double *>(
            reinterpret_cast<const uint8_t *>(arguments) + entry.value_offset);
        out.value.floats.size = entry.value_count;
        break;
    }
    *argument_out = out;
    return 1;
}

} // extern "C"
