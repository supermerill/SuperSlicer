///|/ Copyright (c) SuperSlicer 2026 Durand Rémi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/
#ifndef slic3r_Api_plugin_cpp_ConfigViews_hpp_
#define slic3r_Api_plugin_cpp_ConfigViews_hpp_

#include <cassert>
#include <cstdint>
#include <string>
#include <vector>

#include "libslic3r/Api/plugin/c/slic3r_config_option.h"
#include "libslic3r/Api/plugin/c/slic3r_data_tree.h"

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

} // namespace slic3r_api

#endif // slic3r_Api_plugin_cpp_ConfigViews_hpp_
