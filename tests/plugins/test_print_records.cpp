///|/ Copyright (c) SuperSlicer 2026 Durand Remi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/

/*
Tests for variable record channels owned by Print.

The suite checks the core store, the C ABI and the C++ borrowed view together.
It deliberately mutates channels only from the test thread; the concurrent case
is limited to identifier allocation, which is the only thread-safe write
operation promised by the public contract.
*/

#include <catch2/catch.hpp>

#include <algorithm>
#include <cstdint>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "plugin_test_helpers.hpp"

#include "libslic3r/Api/host/ApiHostUtils.hpp"
#include "libslic3r/Api/plugin/c/slic3r_config.h"
#include "libslic3r/Api/plugin/c/slic3r_data_tree.h"
#include "libslic3r/Api/plugin/cpp/DataTreeViews.hpp"
#include "libslic3r/Config/ConfigDef.hpp"
#include "libslic3r/Config/ConfigOption.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintRecords.hpp"

namespace {

static const print_handle *print_handle_for(const Slic3r::Print &print);

// Convert the host object into the same opaque borrowed handle supplied to a plugin step.
static const print_handle *print_handle_for(const Slic3r::Print &print)
{
    return reinterpret_cast<const print_handle *>(&print);
}

} // namespace

TEST_CASE("Print record store owns free-form Config channels", "[plugins][print-records][core]")
{
    Slic3r::PrintRecordStore records;
    CHECK(records.channels().empty());
    CHECK(records.find("") == nullptr);

    Slic3r::DynamicConfig &generic = records.get_or_add("");
    generic.set_key_value("host.value", new Slic3r::ConfigOptionInt(17));

    Slic3r::DynamicConfig &table = records.get_or_add("example.events");
    table.set_key_value("count", new Slic3r::ConfigOptionInt(2));
    table.set_key_value("record_id", new Slic3r::ConfigOptionInts({1, 2}));
    table.set_key_value("height", new Slic3r::ConfigOptionFloats({0.2, 12.5}));
    table.set_key_value("label", new Slic3r::ConfigOptionStrings({"first", "second"}));

    REQUIRE(records.find("") != nullptr);
    CHECK(records.find("")->option<Slic3r::ConfigOptionInt>("host.value")->value == 17);
    REQUIRE(records.find("example.events") != nullptr);
    CHECK(records.find("example.events")->option<Slic3r::ConfigOptionInt>("count")->value == 2);
    CHECK(records.find("example.events")->option<Slic3r::ConfigOptionInts>("record_id")->get_values() ==
          std::vector<int32_t>{1, 2});
    CHECK(records.find("example.events")->option<Slic3r::ConfigOptionFloats>("height")->get_values() ==
          std::vector<double>{0.2, 12.5});

    CHECK(records.channels() == std::vector<std::string>{"", "example.events"});
    CHECK(records.remove("missing") == false);
    CHECK(records.remove("") == true);
    CHECK(records.find("") == nullptr);
}

TEST_CASE("Print record Config addresses survive channel-map growth", "[plugins][print-records][core]")
{
    Slic3r::PrintRecordStore records;
    Slic3r::DynamicConfig *stable = &records.get_or_add("example.stable");
    stable->set_key_value("value", new Slic3r::ConfigOptionInt(42));

    // unordered_map rehashes may move buckets, but references to node values remain valid.
    for (uint32_t idx = 0; idx < 2048; ++idx)
        records.get_or_add("example.channel." + std::to_string(idx));

    CHECK(records.find_mutable("example.stable") == stable);
    CHECK(stable->option<Slic3r::ConfigOptionInt>("value")->value == 42);
}

TEST_CASE("Print record identifiers are globally unique under concurrency",
          "[plugins][print-records][threading]")
{
    Slic3r::PrintRecordStore records;
    constexpr uint32_t thread_count = 8;
    constexpr uint32_t ids_per_thread = 512;
    std::vector<std::vector<Slic3r::PrintRecordId>> allocated(thread_count);
    std::vector<std::thread> workers;

    for (uint32_t thread_idx = 0; thread_idx < thread_count; ++thread_idx) {
        workers.emplace_back([&records, &allocated, thread_idx]() {
            allocated[thread_idx].reserve(ids_per_thread);
            for (uint32_t idx = 0; idx < ids_per_thread; ++idx)
                allocated[thread_idx].push_back(records.allocate_id());
        });
    }
    for (std::thread &worker : workers)
        worker.join();

    std::set<Slic3r::PrintRecordId> unique;
    for (const std::vector<Slic3r::PrintRecordId> &thread_ids : allocated)
        unique.insert(thread_ids.begin(), thread_ids.end());

    REQUIRE(unique.size() == thread_count * ids_per_thread);
    CHECK(*unique.begin() == 1);
    CHECK(*unique.rbegin() == thread_count * ids_per_thread);
    CHECK(unique.count(Slic3r::INVALID_PRINT_RECORD_ID) == 0);

    records.remove("unrelated");
    CHECK(records.allocate_id() == thread_count * ids_per_thread + 1);
    records.clear();
    CHECK(records.allocate_id() == 1);
}

TEST_CASE("Print record C ABI exposes borrowed mutable Config channels",
          "[plugins][print-records][api]")
{
    Slic3r::Test::Plugins::ensure_plugin_test_runtime_initialized();
    Slic3r::Print print;
    const print_handle *handle = print_handle_for(print);

    CHECK(print_records_channels(nullptr).size == 0);
    CHECK(print_records_get(nullptr, "example") == nullptr);
    CHECK(print_records_get(handle, nullptr) == nullptr);
    CHECK(print_records_get_or_add(nullptr, "example") == nullptr);
    CHECK(print_records_get_or_add(handle, nullptr) == nullptr);
    CHECK(print_records_remove(nullptr, "example") == 0);
    CHECK(print_records_remove(handle, nullptr) == 0);
    CHECK(print_records_allocate_id(nullptr) == PRINT_RECORD_ID_INVALID);

    config_handle *named = print_records_get_or_add(handle, "z.example");
    config_handle *generic = print_records_get_or_add(handle, "");
    REQUIRE(named != nullptr);
    REQUIRE(generic != nullptr);
    CHECK(print_records_get(handle, "z.example") == named);
    CHECK(print_records_get(handle, "missing") == nullptr);

    config_option_handle *value = config_get_or_add_mutable(named, "value", SLIC3R_CONFIG_OPTION_STRING);
    REQUIRE(value != nullptr);
    config_option_set_string(value, "stored", 0);
    CHECK(Slic3r::ApiHost::to_config(print_records_get(handle, "z.example"))
              ->option<Slic3r::ConfigOptionString>("value")->value == "stored");

    const_strings_t channels = print_records_channels(handle);
    REQUIRE(channels.size == 2);
    CHECK(std::string(channels.items[0]) == "");
    CHECK(std::string(channels.items[1]) == "z.example");

    const print_record_id first_id = print_records_allocate_id(handle);
    CHECK(first_id == 1);
    CHECK(print_records_remove(handle, "z.example") == 1);
    CHECK(print_records_remove(handle, "z.example") == 0);
    CHECK(print_records_allocate_id(handle) == 2);

    print.clear();
    CHECK(print_records_channels(handle).size == 0);
    CHECK(print_records_allocate_id(handle) == 1);
}

TEST_CASE("PrintRecords C++ view follows channel lifetime", "[plugins][print-records][cpp]")
{
    Slic3r::Test::Plugins::ensure_plugin_test_runtime_initialized();
    Slic3r::Print print;
    slic3r_api::Print print_view(print_handle_for(print));
    slic3r_api::PrintRecords records = print_view.records();

    CHECK(records.find("example.values").has_value() == false);
    slic3r_api::MutableConfig config = records.get_or_add("example.values");
    config.get_or_add("count", SLIC3R_CONFIG_OPTION_INT).set_int(2);
    config.get_or_add("values", SLIC3R_CONFIG_OPTION_INTS).resize(2);
    config.get_mutable("values").set_int(11, 0);
    config.get_mutable("values").set_int(22, 1);

    std::optional<slic3r_api::Config> found = records.find("example.values");
    REQUIRE(found.has_value());
    CHECK(found->int_or_default("count", 0) == 2);
    CHECK(found->vector_int_or_default("values", 0, 0) == 11);
    CHECK(found->vector_int_or_default("values", 1, 0) == 22);
    CHECK(records.allocate_id() == 1);
    CHECK(records.channels() == std::vector<std::string>{"example.values"});
    CHECK(records.remove("example.values"));
    CHECK_FALSE(records.find("example.values").has_value());
}
