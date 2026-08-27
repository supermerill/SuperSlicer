///|/ Copyright (c) SuperSlicer 2026 Durand Remi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/

/*
Tests for plugin-owned temporary configurations.

The suite exercises the C ABI as a plugin sees it, then uses the C++ wrappers
to verify their ownership contract. The core SCFG codec is covered separately
by libslic3r tests; this suite verifies the ABI buffer and merge semantics.
*/

#include <catch2/catch.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "libslic3r/Api/host/ApiHostUtils.hpp"
#include "libslic3r/Api/host/Orchestrator.hpp"
#include "libslic3r/Api/plugin/c/slic3r_config.h"
#include "libslic3r/Api/plugin/cpp/ConfigViews.hpp"
#include "libslic3r/ConfigDef.hpp"
#include "libslic3r/ConfigOption.hpp"

namespace {

// This small static store exercises the API rejection path without depending
// on the global print-config caches initialized by the larger slicing fixture.
class StaticTestConfig final : public Slic3r::StaticConfig
{
public:
    const Slic3r::ConfigDef *def() const override { return nullptr; }

    const Slic3r::ConfigOption *optptr(const Slic3r::t_config_option_key &key) const override
    {
        return key == "value" ? &m_value : nullptr;
    }

    Slic3r::ConfigOption *optptr(const Slic3r::t_config_option_key &key, bool /* create */) override
    {
        return key == "value" ? &m_value : nullptr;
    }

    Slic3r::t_config_option_keys keys() const override { return {"value"}; }

private:
    Slic3r::ConfigOptionInt m_value{17};
};

static std::string serialize_all(const config_handle *config);

// Use the public two-call protocol so tests also validate byte counts and null
// termination rather than reaching into the host implementation.
static std::string serialize_all(const config_handle *config)
{
    const uint32_t size = config_serialize_all(config, nullptr, 0);
    REQUIRE(size > 0);
    std::string serialized(size + 1, '\0');
    REQUIRE(config_serialize_all(config, &serialized[0], size + 1) == size);
    REQUIRE(serialized[size] == '\0');
    serialized.resize(size);
    return serialized;
}

} // namespace

TEST_CASE("Plugin storage owns temporary configs", "[plugins][config]")
{
    Slic3r::PluginStorage storage;
    storage_handle *storage_api = reinterpret_cast<storage_handle *>(&storage);
    REQUIRE(storage_size(storage_api) == 0);

    config_handle *first = storage_new_config(storage_api);
    REQUIRE(first != nullptr);
    CHECK(is_local_storage(storage_api, first) == 1);
    CHECK(storage_size(storage_api) == 1);

    // StableOwnedVector keeps the first ConfigBase subobject at the same address
    // while later allocations grow the owning vector.
    std::vector<config_handle *> configs;
    for (size_t idx = 0; idx < 32; ++idx)
        configs.push_back(storage_new_config(storage_api));
    CHECK(config_get_or_add_mutable(first, "stable", SLIC3R_CONFIG_OPTION_INT) != nullptr);
    CHECK(storage_size(storage_api) == 33);

    CHECK(storage_free(storage_api, first) == 1);
    CHECK(storage_size(storage_api) == 32);
    storage_clear(storage_api);
    CHECK(storage_size(storage_api) == 0);
}

TEST_CASE("Temporary configs create options and clear only dynamic configs", "[plugins][config]")
{
    Slic3r::PluginStorage storage;
    storage_handle *storage_api = reinterpret_cast<storage_handle *>(&storage);
    config_handle *config = storage_new_config(storage_api);
    REQUIRE(config != nullptr);

    config_option_handle *integer =
        config_get_or_add_mutable(config, "value", SLIC3R_CONFIG_OPTION_INT);
    REQUIRE(integer != nullptr);
    CHECK(config_get_or_add_mutable(config, "value", SLIC3R_CONFIG_OPTION_INT) == integer);
    CHECK(config_get_or_add_mutable(config, "value", SLIC3R_CONFIG_OPTION_STRING) == nullptr);
    CHECK(config_get_or_add_mutable(config, "", SLIC3R_CONFIG_OPTION_INT) == nullptr);
    CHECK(config_get_or_add_mutable(config, "none", SLIC3R_CONFIG_OPTION_NONE) == nullptr);
    CHECK(config_get_or_add_mutable(config, "unknown", static_cast<config_option_type>(999)) == nullptr);

    config_option_set_int(integer, 91, 0);
    CHECK(config_option_get_int(config_get(config, "value"), 0) == 91);
    CHECK(config_clear(config) == 1);
    CHECK(config_keys(config).size == 0);
    CHECK(config_clear(config) == 1);

    StaticTestConfig static_config;
    config_handle *static_handle = Slic3r::ApiHost::to_config_handle(&static_config);
    const size_t static_key_count = static_config.keys().size();
    CHECK(config_clear(static_handle) == 0);
    CHECK(config_get_or_add_mutable(static_handle, "temporary", SLIC3R_CONFIG_OPTION_INT) == nullptr);
    CHECK(static_config.keys().size() == static_key_count);
    CHECK(static_config.option<Slic3r::ConfigOptionInt>("value")->value == 17);
}

TEST_CASE("SCFG C API follows the two-call buffer contract", "[plugins][config][serialization]")
{
    Slic3r::DynamicConfig source;
    source.set_key_value("value", new Slic3r::ConfigOptionString("left:\nright"));
    const std::string serialized = serialize_all(Slic3r::ApiHost::to_config_handle(&source));
    CHECK(serialized.rfind("SCFG1\n", 0) == 0);

    Slic3r::DynamicConfig restored;
    REQUIRE(config_deserialize_all(Slic3r::ApiHost::to_config_handle(&restored), serialized.c_str()) == 1);
    CHECK(restored.option<Slic3r::ConfigOptionString>("value")->value == "left:\nright");
}

TEST_CASE("SCFG merge is atomic and allows an incoming type change", "[plugins][config][serialization]")
{
    Slic3r::DynamicConfig destination;
    destination.set_key_value("keep", new Slic3r::ConfigOptionInt(7));
    destination.set_key_value("replace", new Slic3r::ConfigOptionInt(8));

    Slic3r::DynamicConfig incoming;
    incoming.set_key_value("replace", new Slic3r::ConfigOptionString("changed"));
    incoming.set_key_value("added", new Slic3r::ConfigOptionBool(true));
    const std::string incoming_serialized = serialize_all(Slic3r::ApiHost::to_config_handle(&incoming));

    REQUIRE(config_deserialize_all(
        Slic3r::ApiHost::to_config_handle(&destination), incoming_serialized.c_str()) == 1);
    CHECK(destination.option< Slic3r::ConfigOptionInt>("keep")->value == 7);
    CHECK(destination.option<Slic3r::ConfigOptionString>("replace")->value == "changed");
    CHECK(destination.option<Slic3r::ConfigOptionBool>("added")->value);

    const std::string before_failure = serialize_all(Slic3r::ApiHost::to_config_handle(&destination));
    CHECK(config_deserialize_all(Slic3r::ApiHost::to_config_handle(&destination), "SCFG2\n0\n") == 0);
    CHECK(serialize_all(Slic3r::ApiHost::to_config_handle(&destination)) == before_failure);

    REQUIRE(config_clear(Slic3r::ApiHost::to_config_handle(&destination)) == 1);
    REQUIRE(config_deserialize_all(
        Slic3r::ApiHost::to_config_handle(&destination), incoming_serialized.c_str()) == 1);
    CHECK(destination.keys() == incoming.keys());
}

TEST_CASE("StoredConfig provides move-only C++ ownership", "[plugins][config][cpp]")
{
    Slic3r::PluginStorage storage;
    storage_handle *storage_api = reinterpret_cast<storage_handle *>(&storage);

    {
        slic3r_api::StoredConfig source(storage_api);
        slic3r_api::MutableConfigOption layer =
            source.get_or_add("layer_num", SLIC3R_CONFIG_OPTION_INT);
        layer.set_int(12);
        const std::string serialized = source.serialize_all();

        slic3r_api::StoredConfig destination(storage_api);
        destination.get_or_add("preserved", SLIC3R_CONFIG_OPTION_BOOL).set_bool(true);
        destination.deserialize_all(serialized);
        CHECK(destination.int_or_default("layer_num", 0) == 12);
        CHECK(destination.bool_or_default("preserved", false));

        slic3r_api::StoredConfig moved(std::move(destination));
        CHECK(moved.int_or_default("layer_num", 0) == 12);
        moved.clear();
        CHECK(moved.keys().empty());
    }

    CHECK(storage_size(storage_api) == 0);
}
