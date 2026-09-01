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

struct TestTraversalAuxiliaryProperty
{
    uint32_t touches = 0;
};

struct CallbackRecord
{
    int32_t previous_id = -1;
    int32_t next_id = -1;
    const printing_layer_group_handle *layer = nullptr;
    const printing_tool_group_handle *tool = nullptr;
    const printing_extrusion_handle *extrusion = nullptr;
    const extrusion_entity_handle *entity = nullptr;
    uint16_t extruder_id = UINT16_MAX;
    uint16_t object_instance_idx = UINT16_MAX;
};

/* Resolve one process-local dynamic key for all traversal test plans. */
slic3r_api::PluginPropertyKey<TestTraversalProperty> traversal_property_key();

/* Resolve a second key used to force property-storage changes in callbacks. */
slic3r_api::PluginPropertyKey<TestTraversalAuxiliaryProperty>
traversal_auxiliary_property_key();

/* Build one empty extrusion leaf owned by a PrintingExtrusion. */
Slic3r::ExtrusionEntityUPtr empty_entity();

/* Build a parent with two empty children to exercise depth-first pre-order. */
Slic3r::ExtrusionEntityUPtr nested_entity();

/* Build the fixed four-child shape reserved for a future transition scope. */
Slic3r::ExtrusionEntityUPtr scope_shaped_entity();

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

slic3r_api::PluginPropertyKey<TestTraversalAuxiliaryProperty>
traversal_auxiliary_property_key()
{
    return slic3r_api::PluginPropertyKey<TestTraversalAuxiliaryProperty>::register_dynamic(
        reinterpret_cast<orchestrator_handle *>(
            &Slic3r::Orchestrator::instance()),
        "tests.printing_entity_property_traversal.auxiliary");
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

Slic3r::ExtrusionEntityUPtr scope_shaped_entity()
{
    Slic3r::ExtrusionEntity::Children children;
    for (uint32_t phase_idx = 0; phase_idx < 4; ++phase_idx)
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

    const slic3r_api::PrintingExtrusion middle_extrusion = first_tool.extrusion(1);
    slic3r_api::MutableExtrusionEntity middle_root = middle_extrusion.mutable_root();
    middle_root.get_or_add(key).id = 25;

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
                record.extruder_id = next->tool_group.extruder_id();
                record.object_instance_idx =
                    next->printing_extrusion.object_instance_idx();
            }
            records.emplace_back(record);
        });

    traversal.process(layer);

    REQUIRE(records.size() == 5);
    CHECK(records[0].previous_id == -1);
    CHECK(records[0].next_id == 10);
    CHECK(records[1].previous_id == 10);
    CHECK(records[1].next_id == 20);
    CHECK(records[2].previous_id == 20);
    CHECK(records[2].next_id == 25);
    CHECK(records[3].previous_id == 25);
    CHECK(records[3].next_id == 30);
    CHECK(records[4].previous_id == 30);
    CHECK(records[4].next_id == -1);

    CHECK(records[0].layer == layer.handle());
    CHECK(records[0].tool == first_tool.handle());
    CHECK(records[0].extrusion == first_extrusion.handle());
    CHECK(records[0].entity == first_root.handle());
    CHECK(records[1].tool == first_tool.handle());
    CHECK(records[1].extrusion == first_extrusion.handle());
    CHECK(records[1].entity == nested_child.handle());
    CHECK(records[2].tool == first_tool.handle());
    CHECK(records[2].extrusion == middle_extrusion.handle());
    CHECK(records[2].entity == middle_root.handle());
    CHECK(records[3].tool == second_tool.handle());
    CHECK(records[3].extrusion == last_extrusion.handle());
    CHECK(records[3].entity == last_root.handle());

    CHECK(records[0].extruder_id == 2);
    CHECK(records[0].object_instance_idx == 4);
    CHECK(records[1].extruder_id == 2);
    CHECK(records[1].object_instance_idx == 4);
    CHECK(records[2].extruder_id == 2);
    CHECK(records[2].object_instance_idx == 5);
    CHECK(records[3].extruder_id == 7);
    CHECK(records[3].object_instance_idx == 6);

    CHECK(first_root.get(key)->visits == 2);
    CHECK(nested_child.get(key)->visits == 2);
    CHECK(middle_root.get(key)->visits == 2);
    CHECK(last_root.get(key)->visits == 2);
    CHECK(first_root.child_mutable(0).get(key) == nullptr);
}

TEST_CASE("Printing property traversal permits isolated phase subtree edits",
          "[plugins][printing-plan][property-traversal]")
{
    Slic3r::Test::Plugins::ensure_plugin_test_runtime_initialized();
    Slic3r::Print print;
    Slic3r::Printing::PrintingLayerGroup &core_layer = make_test_layer(print);
    core_layer.tool_groups[0].extrusions.clear();
    core_layer.tool_groups[1].extrusions.clear();
    append_extrusion(core_layer.tool_groups[0], scope_shaped_entity(), 10);
    append_extrusion(core_layer.tool_groups[0], scope_shaped_entity(), 11);

    const slic3r_api::PrintingLayerGroup layer = layer_view(core_layer);
    const slic3r_api::PluginPropertyKey<TestTraversalProperty> key =
        traversal_property_key();
    const slic3r_api::PluginPropertyKey<TestTraversalAuxiliaryProperty> auxiliary_key =
        traversal_auxiliary_property_key();
    slic3r_api::MutableExtrusionEntity first_scope =
        layer.tool_group(0).extrusion(0).mutable_root();
    slic3r_api::MutableExtrusionEntity second_scope =
        layer.tool_group(0).extrusion(1).mutable_root();
    first_scope.get_or_add(key).id = 1;
    second_scope.get_or_add(key).id = 2;

    using Entity = slic3r_api::PrintingEntity<TestTraversalProperty>;
    std::vector<std::pair<int32_t, int32_t>> pairs;
    slic3r_api::PrintingEntityPropertyTraversal<TestTraversalProperty> traversal(
        key,
        [&pairs, &auxiliary_key](Entity *previous, Entity *next) {
            pairs.emplace_back(
                previous != nullptr ? int32_t(previous->property->id) : -1,
                next != nullptr ? int32_t(next->property->id) : -1);

            if (previous != nullptr) {
                previous->entity.get_or_add(auxiliary_key).touches += 1;
                slic3r_api::MutableExtrusionEntity inserted =
                    previous->entity.child_mutable(3).emplace_ordered_leaf(
                        slic3r_api::OrderedLeafPosition::After,
                        slic3r_api::ExistingPropertyPlacement::KeepOnParent);
                if (!inserted.valid())
                    throw std::runtime_error("Unable to edit the previous scope after phase.");
            }
            if (next != nullptr) {
                next->entity.get_or_add(auxiliary_key).touches += 1;
                for (uint32_t phase_idx = 0; phase_idx < 2; ++phase_idx) {
                    slic3r_api::MutableExtrusionEntity inserted =
                        next->entity.child_mutable(phase_idx).emplace_ordered_leaf(
                            slic3r_api::OrderedLeafPosition::After,
                            slic3r_api::ExistingPropertyPlacement::KeepOnParent);
                    if (!inserted.valid())
                        throw std::runtime_error("Unable to edit the next scope entry phase.");
                }
            }
        },
        slic3r_api::MatchingEntityDescendants::Skip);

    traversal.process(layer);

    REQUIRE(pairs.size() == 3);
    CHECK(pairs[0] == std::make_pair(-1, 1));
    CHECK(pairs[1] == std::make_pair(1, 2));
    CHECK(pairs[2] == std::make_pair(2, -1));
    CHECK(first_scope.get(key)->id == 1);
    CHECK(second_scope.get(key)->id == 2);
    CHECK(first_scope.get(auxiliary_key)->touches == 2);
    CHECK(second_scope.get(auxiliary_key)->touches == 2);
    CHECK(first_scope.child_mutable(0).child_count() > 0);
    CHECK(first_scope.child_mutable(1).child_count() > 0);
    CHECK(first_scope.child_mutable(3).child_count() > 0);
    CHECK(second_scope.child_mutable(0).child_count() > 0);
    CHECK(second_scope.child_mutable(1).child_count() > 0);
    CHECK(second_scope.child_mutable(3).child_count() > 0);
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

TEST_CASE("Printing property traversal can skip descendants of a matching entity",
          "[plugins][printing-plan][property-traversal]")
{
    Slic3r::Test::Plugins::ensure_plugin_test_runtime_initialized();
    Slic3r::Print print;
    Slic3r::Printing::PrintingLayerGroup &core_layer = make_test_layer(print);
    const slic3r_api::PrintingLayerGroup layer = layer_view(core_layer);
    const slic3r_api::PluginPropertyKey<TestTraversalProperty> key =
        traversal_property_key();

    slic3r_api::MutableExtrusionEntity marked_root =
        layer.tool_group(0).extrusion(0).mutable_root();
    marked_root.get_or_add(key).id = 10;
    marked_root.child_mutable(1).get_or_add(key).id = 20;
    layer.tool_group(0).extrusion(1).mutable_root().get_or_add(key).id = 30;

    using Entity = slic3r_api::PrintingEntity<TestTraversalProperty>;
    std::vector<std::pair<int32_t, int32_t>> pairs;
    slic3r_api::PrintingEntityPropertyTraversal<TestTraversalProperty> traversal(
        key,
        [&pairs](Entity *previous, Entity *next) {
            pairs.emplace_back(
                previous != nullptr ? int32_t(previous->property->id) : -1,
                next != nullptr ? int32_t(next->property->id) : -1);
        },
        slic3r_api::MatchingEntityDescendants::Skip);

    traversal.process(layer);

    REQUIRE(pairs.size() == 3);
    CHECK(pairs[0] == std::make_pair(-1, 10));
    CHECK(pairs[1] == std::make_pair(10, 30));
    CHECK(pairs[2] == std::make_pair(30, -1));
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
    CHECK_THROWS_AS(
        Traversal(key, [](Entity *, Entity *) {},
                  static_cast<slic3r_api::MatchingEntityDescendants>(255)),
        std::invalid_argument);

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

TEST_CASE("Printing property traversal rejects removal of its selecting property",
          "[plugins][printing-plan][property-traversal]")
{
    Slic3r::Test::Plugins::ensure_plugin_test_runtime_initialized();
    Slic3r::Print print;
    Slic3r::Printing::PrintingLayerGroup &core_layer = make_test_layer(print);
    const slic3r_api::PrintingLayerGroup layer = layer_view(core_layer);
    const slic3r_api::PluginPropertyKey<TestTraversalProperty> key =
        traversal_property_key();
    layer.tool_group(0).extrusion(0).mutable_root().get_or_add(key).id = 1;

    using Entity = slic3r_api::PrintingEntity<TestTraversalProperty>;
    slic3r_api::PrintingEntityPropertyTraversal<TestTraversalProperty> traversal(
        key,
        [&key](Entity *, Entity *next) {
            if (next != nullptr)
                key.remove(next->entity);
        });

    CHECK_THROWS_WITH(
        traversal.process(layer),
        "A traversed entity lost the property used to select it.");
}
