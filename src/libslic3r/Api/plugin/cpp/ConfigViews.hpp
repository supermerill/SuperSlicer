///|/ Copyright (c) SuperSlicer 2026 Durand Rémi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/
#ifndef slic3r_Api_plugin_cpp_ConfigViews_hpp_
#define slic3r_Api_plugin_cpp_ConfigViews_hpp_

#include <cassert>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "libslic3r/Api/plugin/c/slic3r_config.h"
#include "libslic3r/Api/plugin/c/slic3r_config_option.h"

namespace slic3r_api {

struct ConfigPoint
{
    double x = 0.0;
    double y = 0.0;
};

/*
Common borrowed-handle base
===========================

Most plugin C++ views are tiny borrowed wrappers around C ABI handles. This
base stores the handle and asserts that callers do not dereference a null view.
It does not own memory; the host object or storage_handle that produced the
handle controls the lifetime.
*/
template<class Handle> class ConstDataTreeHandleView
{
public:
    ConstDataTreeHandleView() = default;
    explicit ConstDataTreeHandleView(const Handle *handle) : m_handle(handle) { assert(handle != nullptr); }

    const Handle *handle() const {
        assert(m_handle != nullptr);
        return m_handle;
    }

    bool valid() const { return m_handle != nullptr; }

    bool same_handle(const ConstDataTreeHandleView &other) const { return m_handle == other.m_handle; }

protected:
    const Handle *m_handle = nullptr;
};

/*
Config views
============

Config is a borrowed read-only view over one host config object. Use get() when
the option is mandatory and a wrong/missing key should fail fast. Use the
*_or_default() helpers when a plugin can run in contexts where an option is
absent, disabled, or a vector item is disabled.

The helpers return the provided default when:
- the key is missing;
- the option exists but is disabled;
- the vector index is outside the option size;
- the vector item exists but is disabled.

Scalar helpers read item 0. Vector helpers take an explicit item index and are
named vector_*_or_default() so call sites show that they depend on per-item
settings such as extruder or filament arrays.
*/

class ConfigOption : public ConstDataTreeHandleView<config_option_handle>
{
public:
    using ConstDataTreeHandleView<config_option_handle>::ConstDataTreeHandleView;

    config_option_type type() const { return config_option_type_get(handle()); }
    uint32_t size() const { return config_option_size(handle()); }
    bool get_bool(uint32_t idx = 0) const { return config_option_get_bool(handle(), idx) != 0; }
    int32_t get_int(uint32_t idx = 0) const { return config_option_get_int(handle(), idx); }
    double get_float(uint32_t idx = 0) const { return config_option_get_float(handle(), idx); }
    c_float_or_percent get_float_or_percent(uint32_t idx = 0) const {
        return config_option_get_float_or_percent(handle(), idx);
    }
    std::string get_string(uint32_t idx = 0) const {
        const uint32_t needed = config_option_get_string(handle(), idx, nullptr, 0);
        std::string out(needed + 1, '\0');
        if (needed > 0)
            config_option_get_string(handle(), idx, &out[0], needed + 1);
        out.resize(needed);
        return out;
    }
    ConfigPoint get_point(uint32_t idx = 0) const {
        return ConfigPoint{
            config_option_get_float(handle(), idx * 2),
            config_option_get_float(handle(), idx * 2 + 1)
        };
    }
    bool is_percent(uint32_t idx = 0) const {
        return config_option_get_float_or_percent(handle(), idx).percent != 0;
    }
    double get_effective_value(double ratio, uint32_t idx = 0) const {
        c_float_or_percent value = config_option_get_float_or_percent(handle(), idx);
        return c_float_or_percent_get_effective_value(&value, ratio);
    }
    const graph_data_handle *graph(uint32_t idx = 0) const {
        return config_option_get_graph(handle(), idx);
    }
    bool is_enabled(uint32_t idx = 0) const { return config_option_is_enabled(handle(), int32_t(idx)) != 0; }
    bool is_vector() const { return config_option_is_vector(handle()) != 0; }

    // Serialized values are the stable comparison form used by config diffs
    // and by plugin selectors. They are useful when the C++ enum type is not
    // available on the plugin side.
    std::string serialize() const {
        const uint32_t needed = config_option_serialize(handle(), nullptr, 0);
        std::string out(needed + 1, '\0');
        if (needed > 0)
            config_option_serialize(handle(), &out[0], needed + 1);
        out.resize(needed);
        return out;
    }
};

/*
MutableConfigOption is a borrowed mutable view returned by MutableConfig.

It never owns the option. Structural mutations of the parent DynamicConfig may
invalidate the view, so callers should fetch it again after clear() or after a
deserialization that replaces its key.
*/
class MutableConfigOption : public ConfigOption
{
public:
    explicit MutableConfigOption(config_option_handle *handle) : ConfigOption(handle) {}

    config_option_handle *mutable_handle() const {
        return const_cast<config_option_handle *>(handle());
    }

    bool set_enabled(bool enabled, uint32_t idx = 0) const {
        return config_option_set_enabled(mutable_handle(), enabled ? 1 : 0, int32_t(idx)) != 0;
    }
    bool set_can_be_disabled(bool disabled) const {
        return config_option_set_can_be_disabled(mutable_handle(), disabled ? 1 : 0) != 0;
    }
    bool set_phony(bool phony) const {
        return config_option_set_phony(mutable_handle(), phony ? 1 : 0) != 0;
    }
    bool deserialize(const std::string &serialized, bool append = false) const {
        return config_option_deserialize(mutable_handle(), serialized.c_str(), append ? 1 : 0) != 0;
    }
    void set_int(int32_t value, uint32_t idx = 0) const {
        config_option_set_int(mutable_handle(), value, idx);
    }
    void set_float(double value, uint32_t idx = 0) const {
        config_option_set_float(mutable_handle(), value, idx);
    }
    void set_float_or_percent(c_float_or_percent value, uint32_t idx = 0) const {
        config_option_set_float_or_percent(mutable_handle(), value, idx);
    }
    void set_bool(bool value, uint32_t idx = 0) const {
        config_option_set_bool(mutable_handle(), value ? 1 : 0, idx);
    }
    void set_string(const std::string &value, uint32_t idx = 0) const {
        config_option_set_string(mutable_handle(), value.c_str(), idx);
    }
    void resize(uint32_t size) const {
        config_option_vector_handle *vector = config_option_vector_cast_mutable(mutable_handle());
        if (vector == nullptr)
            throw std::runtime_error("Cannot resize a scalar configuration option.");
        config_option_vector_resize(vector, size, nullptr);
    }
    void clear_vector() const {
        config_option_vector_handle *vector = config_option_vector_cast_mutable(mutable_handle());
        if (vector == nullptr)
            throw std::runtime_error("Cannot clear a scalar configuration option as a vector.");
        config_option_vector_clear(vector);
    }
};

class Config : public ConstDataTreeHandleView<config_handle>
{
public:
    using ConstDataTreeHandleView<config_handle>::ConstDataTreeHandleView;

    std::vector<std::string> keys() const {
        std::vector<std::string> out;
        const_strings_t c_keys = config_keys(handle());
        out.reserve(c_keys.size);
        for (uint32_t idx = 0; idx < c_keys.size; ++idx) {
            if (c_keys.items[idx] != nullptr)
                out.emplace_back(c_keys.items[idx]);
        }
        return out;
    }

    // Test whether an optional setting exists before reading it. Some plugin
    // steps can be reused in contexts where a dynamic option or selector was
    // not generated because only one implementation is active.
    bool has(const char *key) const {
        return config_get(handle(), key) != nullptr;
    }

    ConfigOption get(const char *key) const {
        return ConfigOption(config_get(handle(), key));
    }

    // Serialize the whole config rather than one option. The C wrapper uses a
    // sizing call first so the returned string is never truncated.
    std::string serialize_all() const {
        const uint32_t needed = config_serialize_all(handle(), nullptr, 0);
        if (needed == 0)
            throw std::runtime_error("The configuration could not be serialized.");
        std::string out(needed + 1, '\0');
        const uint32_t written = config_serialize_all(handle(), &out[0], needed + 1);
        if (written != needed)
            throw std::runtime_error("The configuration changed while it was being serialized.");
        out.resize(needed);
        return out;
    }

    bool bool_or_default(const char *key, bool fallback) const {
        const config_option_handle *option = option_if_enabled(key, 0);
        return option == nullptr ? fallback : config_option_get_bool(option, 0) != 0;
    }

    bool vector_bool_or_default(const char *key, uint32_t idx, bool fallback) const {
        const config_option_handle *option = vector_option_if_enabled(key, idx);
        return option == nullptr ? fallback : config_option_get_bool(option, idx) != 0;
    }

    int32_t int_or_default(const char *key, int32_t fallback) const {
        const config_option_handle *option = option_if_enabled(key, 0);
        return option == nullptr ? fallback : config_option_get_int(option, 0);
    }

    int32_t vector_int_or_default(const char *key, uint32_t idx, int32_t fallback) const {
        const config_option_handle *option = vector_option_if_enabled(key, idx);
        return option == nullptr ? fallback : config_option_get_int(option, idx);
    }

    int32_t enum_or_default(const char *key, int32_t fallback) const {
        return int_or_default(key, fallback);
    }

    int32_t vector_enum_or_default(const char *key, uint32_t idx, int32_t fallback) const {
        return vector_int_or_default(key, idx, fallback);
    }

    double float_or_default(const char *key, double fallback) const {
        const config_option_handle *option = option_if_enabled(key, 0);
        return option == nullptr ? fallback : config_option_get_float(option, 0);
    }

    double vector_float_or_default(const char *key, uint32_t idx, double fallback) const {
        const config_option_handle *option = vector_option_if_enabled(key, idx);
        return option == nullptr ? fallback : config_option_get_float(option, idx);
    }

    double percent_or_default(const char *key, double fallback) const {
        return float_or_default(key, fallback);
    }

    double vector_percent_or_default(const char *key, uint32_t idx, double fallback) const {
        return vector_float_or_default(key, idx, fallback);
    }

    c_float_or_percent float_or_percent_or_default(const char *key, c_float_or_percent fallback) const {
        const config_option_handle *option = option_if_enabled(key, 0);
        return option == nullptr ? fallback : config_option_get_float_or_percent(option, 0);
    }

    c_float_or_percent vector_float_or_percent_or_default(const char *key,
                                                          uint32_t idx,
                                                          c_float_or_percent fallback) const {
        const config_option_handle *option = vector_option_if_enabled(key, idx);
        return option == nullptr ? fallback : config_option_get_float_or_percent(option, idx);
    }

    double effective_float_or_percent_or_default(const char *key, double ratio, double fallback) const {
        const config_option_handle *option = option_if_enabled(key, 0);
        if (option == nullptr)
            return fallback;
        c_float_or_percent value = config_option_get_float_or_percent(option, 0);
        return c_float_or_percent_get_effective_value(&value, ratio);
    }

    double vector_effective_float_or_percent_or_default(const char *key,
                                                        uint32_t idx,
                                                        double ratio,
                                                        double fallback) const {
        const config_option_handle *option = vector_option_if_enabled(key, idx);
        if (option == nullptr)
            return fallback;
        c_float_or_percent value = config_option_get_float_or_percent(option, idx);
        return c_float_or_percent_get_effective_value(&value, ratio);
    }

    double computed_float_or_default(const char *key, int32_t extruder_id, double fallback) const {
        double value = fallback;
        return config_get_computed_value(handle(), key, extruder_id, &value) != 0 ? value : fallback;
    }

    std::string string_or_default(const char *key, std::string fallback) const {
        const config_option_handle *option = option_if_enabled(key, 0);
        return option == nullptr ? fallback : option_string(option, 0);
    }

    std::string vector_string_or_default(const char *key, uint32_t idx, std::string fallback) const {
        const config_option_handle *option = vector_option_if_enabled(key, idx);
        return option == nullptr ? fallback : option_string(option, idx);
    }

    ConfigPoint point_or_default(const char *key, ConfigPoint fallback) const {
        const config_option_handle *option = option_if_enabled(key, 0);
        return option == nullptr ? fallback : point_from_option(option, 0);
    }

    ConfigPoint vector_point_or_default(const char *key, uint32_t idx, ConfigPoint fallback) const {
        const config_option_handle *option = vector_option_if_enabled(key, idx);
        return option == nullptr ? fallback : point_from_option(option, idx);
    }

    const graph_data_handle *graph_or_default(const char *key, const graph_data_handle *fallback) const {
        const config_option_handle *option = option_if_enabled(key, 0);
        return option == nullptr ? fallback : config_option_get_graph(option, 0);
    }

    const graph_data_handle *vector_graph_or_default(const char *key,
                                                     uint32_t idx,
                                                     const graph_data_handle *fallback) const {
        const config_option_handle *option = vector_option_if_enabled(key, idx);
        return option == nullptr ? fallback : config_option_get_graph(option, idx);
    }

    const config_option_handle *option_if_enabled(const char *key, uint32_t idx) const {
        const config_option_handle *option = config_get(handle(), key);
        if (option == nullptr || config_option_size(option) == 0 || config_option_is_enabled(option, int32_t(idx)) == 0)
            return nullptr;
        return option;
    }

    const config_option_handle *vector_option_if_enabled(const char *key, uint32_t idx) const {
        const config_option_handle *option = config_get(handle(), key);
        if (option == nullptr || idx >= config_option_size(option) ||
            config_option_is_enabled(option, int32_t(idx)) == 0)
            return nullptr;
        return option;
    }

    static std::string option_string(const config_option_handle *option, uint32_t idx) {
        const uint32_t needed = config_option_get_string(option, idx, nullptr, 0);
        std::string out(needed + 1, '\0');
        if (needed > 0)
            config_option_get_string(option, idx, &out[0], needed + 1);
        out.resize(needed);
        return out;
    }

    static ConfigPoint point_from_option(const config_option_handle *option, uint32_t idx) {
        return ConfigPoint{
            config_option_get_float(option, idx * 2),
            config_option_get_float(option, idx * 2 + 1)
        };
    }
};

/*
MutableConfig adds structural operations to a borrowed Config handle.

The host accepts these operations only for DynamicConfig instances. The C++
view converts a rejected operation into an exception so plugin code cannot
mistake a static config for a successfully modified temporary config.
*/
class MutableConfig : public Config
{
public:
    explicit MutableConfig(config_handle *handle) : Config(handle) {}

    config_handle *mutable_handle() const {
        return const_cast<config_handle *>(handle());
    }

    MutableConfigOption get_mutable(const char *key) const {
        config_option_handle *option = config_get_mutable(mutable_handle(), key);
        if (option == nullptr)
            throw std::runtime_error(std::string("Unknown mutable configuration option: ") + key);
        return MutableConfigOption(option);
    }

    MutableConfigOption get_or_add(const char *key, config_option_type type) const {
        config_option_handle *option = config_get_or_add_mutable(mutable_handle(), key, type);
        if (option == nullptr)
            throw std::runtime_error(std::string("Cannot create configuration option: ") + key);
        return MutableConfigOption(option);
    }

    void clear() const {
        if (config_clear(mutable_handle()) == 0)
            throw std::runtime_error("The configuration cannot be cleared.");
    }

    void deserialize_all(const std::string &serialized) const {
        if (config_deserialize_all(mutable_handle(), serialized.c_str()) == 0)
            throw std::runtime_error("The serialized configuration is invalid.");
    }
};

/*
StoredConfig owns one DynamicConfig allocated in a storage_handle.

The wrapper is move-only so exactly one destructor releases the handle. As with
the other Stored* wrappers, the storage must outlive the wrapper and must not be
cleared while the wrapper is still active.
*/
class StoredConfig : public MutableConfig
{
public:
    explicit StoredConfig(storage_handle *storage) :
        MutableConfig(create(storage)), m_storage(storage) {}

    StoredConfig(const StoredConfig &) = delete;
    StoredConfig &operator=(const StoredConfig &) = delete;

    StoredConfig(StoredConfig &&other) noexcept :
        MutableConfig(other.mutable_handle()), m_storage(other.m_storage) {
        other.m_handle = nullptr;
        other.m_storage = nullptr;
    }

    StoredConfig &operator=(StoredConfig &&other) noexcept {
        if (this == &other)
            return *this;
        reset();
        m_handle = other.m_handle;
        m_storage = other.m_storage;
        other.m_handle = nullptr;
        other.m_storage = nullptr;
        return *this;
    }

    ~StoredConfig() { reset(); }

    storage_handle *storage() const { return m_storage; }

    bool free_from_storage() { return reset(); }

private:
    static config_handle *create(storage_handle *storage) {
        if (storage == nullptr)
            throw std::invalid_argument("StoredConfig requires a storage handle.");
        config_handle *handle = storage_new_config(storage);
        if (handle == nullptr)
            throw std::runtime_error("The host could not allocate a temporary configuration.");
        return handle;
    }

    bool reset() noexcept {
        if (m_storage == nullptr || m_handle == nullptr)
            return false;
        const bool freed = storage_free(m_storage, const_cast<config_handle *>(m_handle)) != 0;
        assert(freed);
        m_storage = nullptr;
        m_handle = nullptr;
        return freed;
    }

    storage_handle *m_storage = nullptr;
};

} // namespace slic3r_api

#endif // slic3r_Api_plugin_cpp_ConfigViews_hpp_
