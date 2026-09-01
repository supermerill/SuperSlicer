///|/ Copyright (c) SuperSlicer 2026 Durand Remi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/

/*
PrintingEntityPropertyTraversal tests
=====================================

These tests build small host-owned PrintingPlan hierarchies and inspect them
through the public plugin views. They verify that the traversal streams direct
properties in machine order while preserving the exact layer, tool, extrusion,
and tree-node context supplied to a plugin callback.
*/

#include <catch2/catch.hpp>

#include <cstdint>
#include <memory>
#include <stdexcept>
#include <utility>
#include <vector>

#include "plugin_test_helpers.hpp"

#include "libslic3r/Api/host/Orchestrator.hpp"
#include "libslic3r/Api/plugin/cpp/PrintingEntityPropertyTraversal.hpp"
#include "libslic3r/ExtrusionEntity.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/Printing/PrintingPlan.hpp"

namespace {

struct TestTraversalProperty
{
    uint32_t id = 0;
    uint32_t visits = 0;
};

struct CallbackRecord
{
    int32_t previous_id = -1;
    int32_t next_id = -1;
    const printing_layer_group_handle *layer = nullptr;
    const printing_tool_group_handle *tool = nullptr;
    const printing_extrusion_handle *extrusion = nullptr;
    const extrusion_entity_handle *entity = nullptr;
};

/* Resolve one process-local dynamic key for all traversal test plans. */
slic3r_api::PluginPropertyKey<TestTraversalProperty> traversal_property_key();

/* Build one empty extrusion leaf owned by a PrintingExtrusion. */
Slic3r::ExtrusionEntityUPtr empty_entity();

/* Build a parent with two empty children to exercise depth-first pre-order. */
Slic3r::ExtrusionEntityUPtr nested_entity();

/* Append one core extrusion root to the selected final tool group. */
void append_extrusion(Slic3r::Printing::PrintingToolGroup &tool_group,
                      Slic3r::ExtrusionEntityUPtr root,
                      uint16_t object_instance_idx);

/* Build one layer containing two tools and three independent extrusion roots. */
Slic3r::Printing::PrintingLayerGroup &make_test_layer(Slic3r::Print &print);

/* Wrap one host-owned core layer with its public mutable plugin view. */
slic3r_api::PrintingLayerGroup layer_view(
    Slic3r::Printing::PrintingLayerGroup &layer_group);

slic3r_api::PluginPropertyKey<TestTraversalProperty> traversal_property_key()
{
    return slic3r_api::PluginPropertyKey<TestTraversalProperty>::register_dynamic(
        reinterpret_cast<orchestrator_handle *>(
            &Slic3r::Orchestrator::instance()),
        "tests.printing_entity_property_traversal.marker");
}

Slic3r::ExtrusionEntityUPtr empty_entity()
{
    return std::make_unique<Slic3r::ExtrusionEntity>(false);
}

Slic3r::ExtrusionEntityUPtr nested_entity()
{
    Slic3r::ExtrusionEntity::Children children;
    children.emplace_back(empty_entity());
    children.emplace_back(empty_entity());
    return std::make_unique<Slic3r::ExtrusionEntity>(
        std::move(children), false, false, true);
}

void append_extrusion(Slic3r::Printing::PrintingToolGroup &tool_group,
                      Slic3r::ExtrusionEntityUPtr root,
                      const uint16_t object_instance_idx)
{
    Slic3r::Printing::PrintingExtrusion extrusion;
    extrusion.root = std::move(root);
    extrusion.object_instance_idx = object_instance_idx;
    tool_group.extrusions.emplace_back(std::move(extrusion));
}

Slic3r::Printing::PrintingLayerGroup &make_test_layer(Slic3r::Print &print)
{
    Slic3r::Printing::PrintingPlan &plan = print.mutable_printing_plan();
    plan.groups.emplace_back();
    plan.groups.back().layers.emplace_back();
    Slic3r::Printing::PrintingLayerGroup &layer_group =
        plan.groups.back().layers.back();

    layer_group.tool_groups.emplace_back();
    layer_group.tool_groups.back().extruder_id = 2;
    append_extrusion(layer_group.tool_groups.back(), nested_entity(), 4);
    append_extrusion(layer_group.tool_groups.back(), empty_entity(), 5);

    layer_group.tool_groups.emplace_back();
    layer_group.tool_groups.back().extruder_id = 7;
    append_extrusion(layer_group.tool_groups.back(), empty_entity(), 6);
    return layer_group;
}

slic3r_api::PrintingLayerGroup layer_view(
    Slic3r::Printing::PrintingLayerGroup &layer_group)
{
    return slic3r_api::PrintingLayerGroup(
        reinterpret_cast<printing_layer_group_handle *>(&layer_group));
}

} // namespace

TEST_CASE("Printing property traversal streams direct properties with their owners",
          "[plugins][printing-plan][property-traversal]")
{
    Slic3r::Test::Plugins::ensure_plugin_test_runtime_initialized();
    Slic3r::Print print;
    Slic3r::Printing::PrintingLayerGroup &core_layer = make_test_layer(print);
    const slic3r_api::PrintingLayerGroup layer = layer_view(core_layer);
    const slic3r_api::PluginPropertyKey<TestTraversalProperty> key =
        traversal_property_key();

    const slic3r_api::PrintingToolGroup first_tool = layer.tool_group(0);
    const slic3r_api::PrintingExtrusion first_extrusion = first_tool.extrusion(0);
    slic3r_api::MutableExtrusionEntity first_root = first_extrusion.mutable_root();
    first_root.get_or_add(key).id = 10;
    slic3r_api::MutableExtrusionEntity nested_child = first_root.child_mutable(1);
    nested_child.get_or_add(key).id = 20;

    const slic3r_api::PrintingToolGroup second_tool = layer.tool_group(1);
    const slic3r_api::PrintingExtrusion last_extrusion = second_tool.extrusion(0);
    slic3r_api::MutableExtrusionEntity last_root = last_extrusion.mutable_root();
    last_root.get_or_add(key).id = 30;

    using Entity = slic3r_api::PrintingEntity<TestTraversalProperty>;
    std::vector<CallbackRecord> records;
    slic3r_api::PrintingEntityPropertyTraversal<TestTraversalProperty> traversal(
        key,
        [&records](Entity *previous, Entity *next) {
            CallbackRecord record;
            if (previous != nullptr) {
                record.previous_id = int32_t(previous->property->id);
                ++previous->property->visits;
            }
            if (next != nullptr) {
                record.next_id = int32_t(next->property->id);
                ++next->property->visits;
                record.layer = next->layer_group.handle();
                record.tool = next->tool_group.handle();
                record.extrusion = next->printing_extrusion.handle();
                record.entity = next->entity.handle();
            }
            records.emplace_back(record);
        });

    traversal.process(layer);

    REQUIRE(records.size() == 4);
    CHECK(records[0].previous_id == -1);
    CHECK(records[0].next_id == 10);
    CHECK(records[1].previous_id == 10);
    CHECK(records[1].next_id == 20);
    CHECK(records[2].previous_id == 20);
    CHECK(records[2].next_id == 30);
    CHECK(records[3].previous_id == 30);
    CHECK(records[3].next_id == -1);

    CHECK(records[0].layer == layer.handle());
    CHECK(records[0].tool == first_tool.handle());
    CHECK(records[0].extrusion == first_extrusion.handle());
    CHECK(records[0].entity == first_root.handle());
    CHECK(records[1].tool == first_tool.handle());
    CHECK(records[1].extrusion == first_extrusion.handle());
    CHECK(records[1].entity == nested_child.handle());
    CHECK(records[2].tool == second_tool.handle());
    CHECK(records[2].extrusion == last_extrusion.handle());
    CHECK(records[2].entity == last_root.handle());

    CHECK(first_root.get(key)->visits == 2);
    CHECK(nested_child.get(key)->visits == 2);
    CHECK(last_root.get(key)->visits == 2);
}

TEST_CASE("Printing property traversal is independent for every layer call",
          "[plugins][printing-plan][property-traversal]")
{
    Slic3r::Test::Plugins::ensure_plugin_test_runtime_initialized();
    Slic3r::Print print;
    Slic3r::Printing::PrintingLayerGroup &core_layer = make_test_layer(print);
    const slic3r_api::PrintingLayerGroup layer = layer_view(core_layer);
    const slic3r_api::PluginPropertyKey<TestTraversalProperty> key =
        traversal_property_key();

    using Entity = slic3r_api::PrintingEntity<TestTraversalProperty>;
    uint32_t callback_count = 0;
    slic3r_api::PrintingEntityPropertyTraversal<TestTraversalProperty> traversal(
        key,
        [&callback_count](Entity *, Entity *) { ++callback_count; });

    traversal.process(layer);
    CHECK(callback_count == 0);

    layer.tool_group(0).extrusion(1).mutable_root().get_or_add(key).id = 42;
    traversal.process(layer);
    CHECK(callback_count == 2);
    traversal.process(layer);
    CHECK(callback_count == 4);
}

TEST_CASE("Printing property traversal reports invalid use and callback errors",
          "[plugins][printing-plan][property-traversal]")
{
    Slic3r::Test::Plugins::ensure_plugin_test_runtime_initialized();
    const slic3r_api::PluginPropertyKey<TestTraversalProperty> key =
        traversal_property_key();
    using Traversal =
        slic3r_api::PrintingEntityPropertyTraversal<TestTraversalProperty>;
    using Entity = slic3r_api::PrintingEntity<TestTraversalProperty>;

    CHECK_THROWS_AS(
        Traversal(key, Traversal::ProcessEntity()), std::invalid_argument);

    const slic3r_api::PrintingLayerGroup invalid_layer(
        static_cast<printing_layer_group_handle *>(nullptr));
    Traversal valid_traversal(key, [](Entity *, Entity *) {});
    CHECK_THROWS_AS(valid_traversal.process(invalid_layer), std::invalid_argument);

    Slic3r::Print print;
    Slic3r::Printing::PrintingLayerGroup &core_layer = make_test_layer(print);
    const slic3r_api::PrintingLayerGroup layer = layer_view(core_layer);
    layer.tool_group(0).extrusion(0).mutable_root().get_or_add(key).id = 1;
    Traversal throwing_traversal(
        key,
        [](Entity *, Entity *) {
            throw std::runtime_error("expected traversal callback failure");
        });
    CHECK_THROWS_WITH(
        throwing_traversal.process(layer), "expected traversal callback failure");
}
