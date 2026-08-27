///|/ Copyright (c) SuperSlicer 2026 Durand Remi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/

/*
Tests for plugin-owned temporary configurations.

The suite exercises the C ABI as a plugin sees it, then uses the C++ wrappers
to verify their ownership contract. Serialization tests construct every public
option kind so additions to config_option_type cannot silently become
unserializable.
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
static void add_all_option_types(Slic3r::DynamicConfig &config);
static void compare_configs(const Slic3r::DynamicConfig &expected,
                            const Slic3r::DynamicConfig &actual);

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

// Populate one representative value of every type. Several options also carry
// non-default flags to prove that the outer SCFG record preserves metadata in
// addition to the option's own serialized value.
static void add_all_option_types(Slic3r::DynamicConfig &config)
{
    config.set_key_value("float", new Slic3r::ConfigOptionFloat(1.25));
    config.set_key_value("floats", new Slic3r::ConfigOptionFloats({1.25, -3.5}));
    config.set_key_value("int", new Slic3r::ConfigOptionInt(-17));
    config.set_key_value("ints", new Slic3r::ConfigOptionInts({-17, 42}));
    config.set_key_value("string", new Slic3r::ConfigOptionString("left:\nright;value"));

    Slic3r::ConfigOptionStrings *strings = new Slic3r::ConfigOptionStrings({"first", "second\nline"});
    strings->set_is_extruder_size(true);
    strings->set_can_be_disabled();
    strings->set_enabled(false, 1);
    strings->flags |= FCO_PLACEHOLDER_TEMP;
    config.set_key_value("strings", strings);

    config.set_key_value("percent", new Slic3r::ConfigOptionPercent(35.0));
    config.set_key_value("percents", new Slic3r::ConfigOptionPercents({20.0, 80.0}));
    config.set_key_value("float_or_percent", new Slic3r::ConfigOptionFloatOrPercent(55.0, true));
    config.set_key_value(
        "floats_or_percents",
        new Slic3r::ConfigOptionFloatsOrPercents({{2.5, false}, {75.0, true}}));
    config.set_key_value("point", new Slic3r::ConfigOptionPoint(Slic3r::Vec2d(1.5, 2.5)));
    config.set_key_value(
        "points",
        new Slic3r::ConfigOptionPoints({Slic3r::Vec2d(1.0, 2.0), Slic3r::Vec2d(3.0, 4.0)}));
    config.set_key_value("point3", new Slic3r::ConfigOptionPoint3(Slic3r::Vec3d(1.0, 2.0, 3.0)));
    config.set_key_value("bool", new Slic3r::ConfigOptionBool(true));
    config.set_key_value("bools", new Slic3r::ConfigOptionBools({true, false, true}));

    Slic3r::ConfigOptionEnumGeneric *enum_option = new Slic3r::ConfigOptionEnumGeneric(nullptr, 23);
    enum_option->set_phony(true);
    config.set_key_value("enum", enum_option);

    config.set_key_value("graph", new Slic3r::ConfigOptionGraph(Slic3r::GraphData()));
    config.set_key_value(
        "graphs",
        new Slic3r::ConfigOptionGraphs({Slic3r::GraphData(), Slic3r::GraphData()}));
}

// Enum text requires a ConfigDef label map, so temporary enums are compared by
// number. Every other option can use its established canonical serialization.
static void compare_configs(const Slic3r::DynamicConfig &expected,
                            const Slic3r::DynamicConfig &actual)
{
    REQUIRE(actual.keys() == expected.keys());
    for (const std::string &key : expected.keys()) {
        const Slic3r::ConfigOption *expected_option = expected.option(key);
        const Slic3r::ConfigOption *actual_option = actual.option(key);
        REQUIRE(expected_option != nullptr);
        REQUIRE(actual_option != nullptr);
        CHECK(actual_option->type() == expected_option->type());
        CHECK(actual_option->flags == expected_option->flags);
        if (expected_option->type() == Slic3r::coEnum)
            CHECK(actual_option->get_int() == expected_option->get_int());
        else
            CHECK(actual_option->serialize() == expected_option->serialize());
    }
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

TEST_CASE("SCFG round-trips every public config option type", "[plugins][config][serialization]")
{
    Slic3r::DynamicConfig empty;
    const std::string serialized_empty = serialize_all(Slic3r::ApiHost::to_config_handle(&empty));
    CHECK(serialized_empty == "SCFG1\n0\n");
    Slic3r::DynamicConfig restored_empty;
    REQUIRE(config_deserialize_all(
        Slic3r::ApiHost::to_config_handle(&restored_empty), serialized_empty.c_str()) == 1);
    CHECK(restored_empty.empty());

    Slic3r::DynamicConfig source;
    add_all_option_types(source);
    const std::string serialized = serialize_all(Slic3r::ApiHost::to_config_handle(&source));
    CHECK(serialized.rfind("SCFG1\n", 0) == 0);

    Slic3r::DynamicConfig restored;
    REQUIRE(config_deserialize_all(Slic3r::ApiHost::to_config_handle(&restored), serialized.c_str()) == 1);
    compare_configs(source, restored);

    // The map order, not insertion order, defines the document bytes.
    Slic3r::DynamicConfig ordered_first;
    ordered_first.set_key_value("a", new Slic3r::ConfigOptionInt(1));
    ordered_first.set_key_value("z", new Slic3r::ConfigOptionString("last"));
    Slic3r::DynamicConfig ordered_second;
    ordered_second.set_key_value("z", new Slic3r::ConfigOptionString("last"));
    ordered_second.set_key_value("a", new Slic3r::ConfigOptionInt(1));
    CHECK(serialize_all(Slic3r::ApiHost::to_config_handle(&ordered_first)) ==
          serialize_all(Slic3r::ApiHost::to_config_handle(&ordered_second)));
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
    std::vector<std::string> invalid_documents = {
        "SCFG2\n0\n",
        "SCFG1\n1\n999:8:1:1\na1",
        "SCFG1\n1\n2:8:4:1\nkey",
        "SCFG1\n1\n2:8:1:10\nanot-an-int",
        "SCFG1\n1\n2:4294967295:1:1\na1",
        "SCFG1\n2\n2:8:1:1\na12:8:1:1\na2",
        incoming_serialized + "trailing"
    };
    for (const std::string &invalid : invalid_documents) {
        CHECK(config_deserialize_all(Slic3r::ApiHost::to_config_handle(&destination), invalid.c_str()) == 0);
        CHECK(serialize_all(Slic3r::ApiHost::to_config_handle(&destination)) == before_failure);
    }

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
