///|/ Copyright (c) SuperSlicer 2026 Durand Rémi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/
#include "Plugin.hpp"

#include <stdexcept>
#include <string>

namespace Slic3r {

namespace {

const int32_t MAX_PLUGIN_CONFIG_KEYS = 1024;

void validate_plugin_instance(const plugin_instance &c_api)
{
    if (c_api.vt == nullptr)
        throw std::runtime_error("Plugin instance has no vtable.");
    if (c_api.vt->abi_version != SLIC3R_PLUGIN_ABI_VERSION)
        throw std::runtime_error(
            "Plugin ABI version mismatch: plugin ABI " + std::to_string(c_api.vt->abi_version) +
            ", host ABI " + std::to_string(SLIC3R_PLUGIN_ABI_VERSION) +
            ". Plugin id is unavailable because the vtable layout may be incompatible.");
    if (c_api.vt->get_id == nullptr || c_api.vt->get_name == nullptr ||
        c_api.vt->get_description == nullptr ||
        c_api.vt->get_exclusive_group == nullptr ||
        c_api.vt->get_exclusive_group_label == nullptr ||
        c_api.vt->get_exclusive_group_tooltip == nullptr ||
        c_api.vt->get_step == nullptr ||
        c_api.vt->get_dependencies == nullptr || c_api.vt->get_priority == nullptr ||
        c_api.vt->used_config_keys == nullptr || c_api.vt->defined_config_keys == nullptr ||
        c_api.vt->initialize == nullptr || c_api.vt->setup == nullptr ||
        c_api.vt->setup_run == nullptr || c_api.vt->run == nullptr)
        throw std::runtime_error("Plugin vtable has missing callbacks.");
}

void read_plugin_used_config_keys(const plugin_instance &c_api,
                                  plugin_used_config_keys_fn callback,
                                  std::vector<Plugin::UsedConfigKey> &out)
{
    // Plugin key lists use a C-style double-call convention. The cap prevents a
    // broken plugin from reserving an arbitrary amount of memory during startup.
    const int32_t key_count = callback(c_api.ctx, nullptr);
    if (key_count < 0 || key_count > MAX_PLUGIN_CONFIG_KEYS)
        throw std::runtime_error("Plugin returned an invalid used_config_keys count.");

    if (key_count == 0)
        return;

    std::vector<raw_used_config_key> keys;
    keys.assign(size_t(key_count), raw_used_config_key{});
    const int32_t written_count = callback(c_api.ctx, keys.data());
    if (written_count < 0 || written_count > key_count)
        throw std::runtime_error("Plugin wrote an invalid used_config_keys count.");

    for (int32_t i = 0; i < written_count; ++i) {
        const raw_used_config_key &key = keys[size_t(i)];
        if (key.key == nullptr)
            continue;
        if (key.type == RAW_CO_NONE)
            throw std::runtime_error(std::string("Plugin declares used config key '") + key.key +
                                     "' without a value type.");
        out.push_back(Plugin::UsedConfigKey{
            key.key,
            key.type,
            key.container_type,
            key.option_preset_type
        });
    }
}

void read_plugin_defined_config_keys(const plugin_instance &c_api,
                                     plugin_defined_config_keys_fn callback,
                                     const std::vector<Plugin::UsedConfigKey> &used_keys,
                                     std::vector<Plugin::DefinedConfigKey> &out)
{
    const int32_t key_count = callback(c_api.ctx, nullptr);
    if (key_count < 0 || key_count > MAX_PLUGIN_CONFIG_KEYS)
        throw std::runtime_error("Plugin returned an invalid defined_config_keys count.");

    if (key_count == 0)
        return;

    std::vector<const char *> keys;
    keys.assign(size_t(key_count), nullptr);
    const int32_t written_count = callback(c_api.ctx, keys.data());
    if (written_count < 0 || written_count > key_count)
        throw std::runtime_error("Plugin wrote an invalid defined_config_keys count.");

    // Defined keys intentionally reuse the richer used-key declaration. This
    // keeps the lightweight activation callback ABI stable while making a
    // setting's type available before an inactive plugin is initialized.
    for (int32_t i = 0; i < written_count; ++i) {
        const char *defined_key = keys[size_t(i)];
        if (defined_key == nullptr)
            continue;

        const Plugin::UsedConfigKey *matching_used_key = nullptr;
        for (const Plugin::UsedConfigKey &used_key : used_keys) {
            if (used_key.key != defined_key)
                continue;
            if (matching_used_key != nullptr && matching_used_key->type != used_key.type)
                throw std::runtime_error(
                    std::string("Plugin declares defined config key '") + defined_key +
                    "' with conflicting used_config_keys types.");
            matching_used_key = &used_key;
        }

        if (matching_used_key == nullptr)
            throw std::runtime_error(
                std::string("Plugin declares defined config key '") + defined_key +
                "' without a matching used_config_keys entry.");

        out.push_back(Plugin::DefinedConfigKey{defined_key, matching_used_key->type});
    }
}

} // namespace

Plugin::Plugin(plugin_instance c_api,
               std::string default_translation_domain,
               std::string package_root)
    : m_c_api(c_api)
    , m_package_root(std::move(package_root))
{
    validate_plugin_instance(c_api);

    const char *plugin_id = c_api.vt->get_id(c_api.ctx);
    const char *plugin_name = c_api.vt->get_name(c_api.ctx);
    const char *plugin_description = c_api.vt->get_description(c_api.ctx);
    const char *plugin_exclusive_group = c_api.vt->get_exclusive_group(c_api.ctx);
    const char *plugin_exclusive_group_label = c_api.vt->get_exclusive_group_label(c_api.ctx);
    const char *plugin_exclusive_group_tooltip = c_api.vt->get_exclusive_group_tooltip(c_api.ctx);
    this->m_id = plugin_id != nullptr ? plugin_id : "";
    this->m_translation_domain = std::move(default_translation_domain);
    if (this->m_translation_domain.empty())
        this->m_translation_domain = this->m_id;
    this->m_name = plugin_name != nullptr && plugin_name[0] != '\0' ? plugin_name : this->m_id;
    this->m_description = plugin_description != nullptr ? plugin_description : "";
    this->m_exclusive_group = plugin_exclusive_group != nullptr && plugin_exclusive_group[0] != '\0' ?
        plugin_exclusive_group :
        this->m_id;
    this->m_exclusive_group_label = plugin_exclusive_group_label != nullptr ? plugin_exclusive_group_label : "";
    this->m_exclusive_group_tooltip = plugin_exclusive_group_tooltip != nullptr ? plugin_exclusive_group_tooltip : "";
    this->m_step = c_api.vt->get_step(c_api.ctx);
    this->m_priority = c_api.vt->get_priority(c_api.ctx);
    const_strings_t cstrings = c_api.vt->get_dependencies(c_api.ctx);
    if (cstrings.items) {
        for (size_t i = 0; i < cstrings.size; ++i) {
            m_dependencies.emplace_back(cstrings.items[i]);
        }
    }

    read_plugin_used_config_keys(c_api, c_api.vt->used_config_keys, m_used_config_keys);
    read_plugin_defined_config_keys(
        c_api, c_api.vt->defined_config_keys, m_used_config_keys, m_defined_config_keys);
}

} // namespace Slic3r
