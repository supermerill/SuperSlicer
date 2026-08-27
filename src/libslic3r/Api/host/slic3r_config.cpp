///|/ Copyright (c) SuperSlicer 2026 Durand Remi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/

/*
Host adapter for plugin configuration handles.

This file translates the C ABI into the core configuration model, owns the
two-call string-buffer convention and applies decoded values atomically. The
versioned SCFG snapshot format is implemented independently in
ConfigSnapshotSerialization so it
can be tested and reused without plugin handles or orchestrator state.
*/

#include "libslic3r/Api/plugin/c/slic3r_config.h"

#include <cstring>
#include <limits>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "ApiHostUtils.hpp"
#include "Orchestrator.hpp"
#include "libslic3r/ConfigDef.hpp"
#include "libslic3r/ConfigSnapshotSerialization.hpp"

namespace Slic3r {

static uint32_t copy_string_out(const std::string &value, char *output, uint32_t capacity);

// Implement the C ABI's two-call string convention while always writing a
// terminator when the caller supplies a non-empty output buffer.
static uint32_t copy_string_out(const std::string &value, char *output, uint32_t capacity)
{
    if (value.size() > std::numeric_limits<uint32_t>::max())
        return 0;
    if (output != nullptr && capacity > 0) {
        const uint32_t count = value.size() < capacity - 1 ? static_cast<uint32_t>(value.size()) : capacity - 1;
        if (count > 0)
            std::memcpy(output, value.data(), count);
        output[count] = '\0';
    }
    return static_cast<uint32_t>(value.size());
}

} // namespace Slic3r

extern "C" {

const_strings_t config_keys(const config_handle *config_handle_value)
{
    const_strings_t out = {};
    if (config_handle_value == nullptr)
        return out;

    // The C array points into thread-local storage so callers may consume it
    // after this function returns without allocating or transferring ownership.
    static thread_local std::vector<std::string> key_storage;
    static thread_local std::vector<const char *> key_ptrs;
    key_storage = Slic3r::ApiHost::to_config(config_handle_value)->keys();
    key_ptrs.clear();
    key_ptrs.reserve(key_storage.size());
    for (const std::string &key : key_storage)
        key_ptrs.push_back(key.c_str());

    out.items = key_ptrs.empty() ? nullptr : key_ptrs.data();
    out.size = key_ptrs.size();
    return out;
}

const config_option_handle *config_get(const config_handle *config_handle_value, const char *key)
{
    if (config_handle_value == nullptr || key == nullptr)
        return nullptr;
    const Slic3r::ConfigBase *config = Slic3r::ApiHost::to_config(config_handle_value);
    return reinterpret_cast<const config_option_handle *>(config->option(key));
}

config_option_handle *config_get_mutable(config_handle *config_handle_value, const char *key)
{
    if (config_handle_value == nullptr || key == nullptr)
        return nullptr;
    Slic3r::ConfigBase *config = Slic3r::ApiHost::to_config(config_handle_value);
    return reinterpret_cast<config_option_handle *>(config->optptr(key, false));
}

config_handle *storage_new_config(storage_handle *storage_handle_value)
{
    if (storage_handle_value == nullptr)
        return nullptr;
    try {
        Slic3r::PluginStorage *storage = reinterpret_cast<Slic3r::PluginStorage *>(storage_handle_value);
        Slic3r::DynamicConfig &config = storage->configs.emplace_back();
        config_handle *handle = Slic3r::ApiHost::to_config_handle(&config);
        storage->generic_storage.insert(handle);
        return handle;
    } catch (...) {
        return nullptr;
    }
}

config_option_handle *config_get_or_add_mutable(config_handle *config_handle_value,
                                                const char *key,
                                                config_option_type type)
{
    if (config_handle_value == nullptr || key == nullptr || key[0] == '\0' ||
        !Slic3r::ConfigSnapshotSerialization::Detail::supports_option_type(type))
        return nullptr;
    try {
        Slic3r::DynamicConfig *config =
            dynamic_cast<Slic3r::DynamicConfig *>(Slic3r::ApiHost::to_config(config_handle_value));
        if (config == nullptr)
            return nullptr;

        Slic3r::ConfigOption *existing = config->optptr(key, false);
        if (existing != nullptr)
            return existing->type() == static_cast<Slic3r::ConfigOptionType>(type) ?
                reinterpret_cast<config_option_handle *>(existing) : nullptr;

        std::unique_ptr<Slic3r::ConfigOption> option =
            Slic3r::ConfigSnapshotSerialization::Detail::create_empty_option(type);
        if (option == nullptr)
            return nullptr;
        Slic3r::ConfigOption *result = option.get();
        config->set_key_value(key, option.release());
        return reinterpret_cast<config_option_handle *>(result);
    } catch (...) {
        return nullptr;
    }
}

int32_t config_clear(config_handle *config_handle_value)
{
    if (config_handle_value == nullptr)
        return 0;
    try {
        Slic3r::DynamicConfig *config =
            dynamic_cast<Slic3r::DynamicConfig *>(Slic3r::ApiHost::to_config(config_handle_value));
        if (config == nullptr)
            return 0;
        config->clear();
        return 1;
    } catch (...) {
        return 0;
    }
}

uint32_t config_serialize_all(const config_handle *config_handle_value, char *output, uint32_t capacity)
{
    if (config_handle_value == nullptr)
        return 0;
    try {
        std::string serialized;
        if (!Slic3r::ConfigSnapshotSerialization::serialize_all(
                *Slic3r::ApiHost::to_config(config_handle_value), serialized))
            return 0;
        return Slic3r::copy_string_out(serialized, output, capacity);
    } catch (...) {
        return 0;
    }
}

int32_t config_deserialize_all(config_handle *destination_handle, const char *serialized_value)
{
    if (destination_handle == nullptr || serialized_value == nullptr)
        return 0;
    try {
        Slic3r::DynamicConfig *destination =
            dynamic_cast<Slic3r::DynamicConfig *>(Slic3r::ApiHost::to_config(destination_handle));
        if (destination == nullptr)
            return 0;

        Slic3r::DynamicConfig incoming;
        if (!Slic3r::ConfigSnapshotSerialization::deserialize_all(serialized_value, incoming))
            return 0;

        // Clone the destination and replace only incoming keys. A type change
        // is valid during a merge, so set_key_value() is used instead of the
        // type-preserving DynamicConfig operator+=.
        Slic3r::DynamicConfig merged(*destination);
        for (std::map<Slic3r::t_config_option_key,
                      std::unique_ptr<Slic3r::ConfigOption>>::const_iterator it = incoming.cbegin();
             it != incoming.cend(); ++it)
            merged.set_key_value(it->first, it->second->clone());
        destination->swap(merged);
        return 1;
    } catch (...) {
        return 0;
    }
}

} // extern "C"
