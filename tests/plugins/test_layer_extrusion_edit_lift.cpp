///|/ Copyright (c) SuperSlicer 2026 Durand Remi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/

/*
CreateRetractionLift tests
==========================

These cases run the real compact transition pipeline. They inspect Z offsets
on the reserved travel and wipe phases, which verifies both the lift profile
and the rule that a layer worker never has to rebuild printable scope content.
*/

#include "layer_extrusion_edit_transition_test_helpers.hpp"

#include <algorithm>
#include <optional>
#include <vector>

namespace {

using namespace Slic3r;
using namespace Slic3r::Printing;
using namespace Slic3r::Test::TransitionPipeline;
using slic3r_api::LayerExtrusionEdit::ExtrusionScope::OrderedExtrusionScope;

constexpr const char *WIPE_PLUGIN = "layer_extrusion_edit.wipe.default";
constexpr const char *ENTRY_STATE_PLUGIN =
    "layer_extrusion_edit.entry_state.default";
constexpr const char *TRAVEL_PLUGIN = "layer_extrusion_edit.travel.default";
constexpr const char *LIFT_PLUGIN = "layer_extrusion_edit.lift.default";

/* One geometric leaf with the process role effective at that leaf. */
struct LiftLeaf
{
    slic3r_api::ExtrusionEntity entity;
    raw_extrusion_role role = RAW_EXTRUSION_ROLE_NONE;
};

/* Build the two-path fixture with all lift settings applied before Print::apply. */
void prepare_lift_print(Print &print,
                        Model &model,
                        const char *lift,
                        const char *ramping,
                        const char *slope,
                        const char *minimum_travel = "0",
                        bool wipe_lift = false);

/* Collect geometric leaves while resolving inherited process attributes. */
void collect_lift_leaves(
    const slic3r_api::ExtrusionEntity &entity,
    std::optional<slic3r_api::EPropertyAttributes> inherited_attributes,
    std::vector<LiftLeaf> &leaves);

void prepare_lift_print(
    Print &print,
    Model &model,
    const char *lift,
    const char *ramping,
    const char *slope,
    const char *minimum_travel,
    const bool wipe_lift)
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({
        {"retract_before_travel", "0"},
        {"retract_layer_change", "0"},
        {"retract_length", "2"},
        {"retract_length_toolchange", "2"},
        {"retract_restart_extra", "0.1"},
        {"retract_restart_extra_toolchange", "0.2"},
        {"retract_lift", lift},
        {"retract_lift_above", "0"},
        {"retract_lift_below", "0"},
        {"retract_lift_first_layer", "0"},
        {"retract_lift_top", "All surfaces"},
        {"retract_lift_before_travel", minimum_travel},
        {"travel_ramping_lift", ramping},
        {"travel_slope", slope},
        {"wipe", wipe_lift ? "1" : "0"},
        {"wipe_speed", "20"},
        {"wipe_min", "8"},
        {"wipe_return", "0"},
        {"wipe_only_crossing", "0"},
        {"retract_before_wipe", "20%"},
        {"retract_speed", "40"},
        {"travel_speed", "120"},
        {"wipe_inside_start", "0"},
        {"wipe_inside_end", "0"},
        {"wipe_extra_perimeter", "0"},
        {"wipe_lift", wipe_lift ? "1" : "0"},
        {"wipe_lift_length", wipe_lift ? "100%" : "0"}
    });
    Slic3r::Test::init_print(
        {Slic3r::Test::TestMesh::cube_20x20x20}, print, model, config);

    PrintingPlan &plan = print.mutable_printing_plan();
    plan.groups.clear();
    plan.groups.emplace_back();
    plan.groups.back().layers.emplace_back();
    PrintingLayerGroup &layer = plan.groups.back().layers.back();
    layer.print_z = scale_i(0.2);
    layer.tool_groups.emplace_back();
    PrintingToolGroup &tool = layer.tool_groups.back();
    tool.extruder_id = 0;
    append_printing_extrusion(tool, make_print_path(0.0, 10.0));
    append_printing_extrusion(tool, make_print_path(20.0, 30.0));
}

void collect_lift_leaves(
    const slic3r_api::ExtrusionEntity &entity,
    std::optional<slic3r_api::EPropertyAttributes> inherited_attributes,
    std::vector<LiftLeaf> &leaves)
{
    if (const slic3r_api::EPropertyAttributes *direct =
            entity.get(slic3r_api::EPropertyAttributes::key))
        inherited_attributes = *direct;
    if (entity.segment_count() != 0 && inherited_attributes)
        leaves.push_back(LiftLeaf{entity, inherited_attributes->extrusion_role()});
    for (uint32_t child_idx = 0; child_idx < entity.child_count(); ++child_idx)
        collect_lift_leaves(entity.child(child_idx), inherited_attributes, leaves);
}

TEST_CASE("CreateRetractionLift registers after travel routing",
          "[plugins][layer-extrusion-edit][lift]")
{
    Slic3r::Test::Plugins::ensure_plugin_test_runtime_initialized();
    Plugin *plugin = Orchestrator::instance().get_plugin(LIFT_PLUGIN);
    REQUIRE(plugin != nullptr);
    CHECK(plugin->get_step() == STEP_LAYER_EXTRUSION_EDIT);
    CHECK(plugin->get_priority() == -40);
    CHECK(plugin->get_exclusive_group() == "layer_extrusion_edit.lift");
    REQUIRE(plugin->get_dependencies().size() == 1);
    CHECK(plugin->get_dependencies().front() == TRANSITION_SCOPE_PLUGIN);

    const std::vector<Plugin::UsedConfigKey> keys = plugin->get_used_config_keys();
    CHECK(std::any_of(keys.begin(), keys.end(), [](const Plugin::UsedConfigKey &key) {
        return key.key == "retract_lift" && key.type == RAW_CO_VECTOR_FLOAT;
    }));
    CHECK(std::any_of(keys.begin(), keys.end(), [](const Plugin::UsedConfigKey &key) {
        return key.key == "wipe_lift" &&
            key.type == RAW_CO_VECTOR_FLOAT_OR_PERCENT;
    }));
}

TEST_CASE("CreateRetractionLift creates a symmetric ramped travel",
          "[plugins][layer-extrusion-edit][lift][travel]")
{
    Slic3r::Test::Plugins::ensure_plugin_test_runtime_initialized();
    Print print;
    Model model;
    prepare_lift_print(print, model, "1", "1", "0");

    run_layer_plugins(print, {
        TRANSITION_SCOPE_PLUGIN, RETRACTION_PLUGIN, ENTRY_STATE_PLUGIN,
        TRAVEL_PLUGIN, LIFT_PLUGIN
    });

    PrintingToolGroup &tool = print.mutable_printing_plan().groups.front()
        .layers.front().tool_groups.front();
    slic3r_api::MutableExtrusionEntity target = find_scope(
        entity_view(*tool.extrusions[1].root));
    REQUIRE(target.valid());
    OrderedExtrusionScope scope(target, scope_property_key());
    const slic3r_api::MutableExtrusionEntity travel = scope.travel();
    REQUIRE(travel.child_count() == 2);
    const slic3r_api::ExtrusionEntity ascending = travel.child(0);
    const slic3r_api::ExtrusionEntity descending = travel.child(1);
    REQUIRE(ascending.point_count() >= 2);
    REQUIRE(descending.point_count() >= 2);
    CHECK(ascending.z_offset(0) == 0);
    CHECK(ascending.z_offset(ascending.point_count() - 1) == scale_i(1.0));
    CHECK(descending.z_offset(0) == scale_i(1.0));
    CHECK(descending.z_offset(descending.point_count() - 1) == 0);
}

TEST_CASE("CreateRetractionLift carries source lift across an empty layer",
          "[plugins][layer-extrusion-edit][lift][parallel]")
{
    Slic3r::Test::Plugins::ensure_plugin_test_runtime_initialized();
    Print print;
    Model model;
    prepare_lift_print(print, model, "0.6", "1", "0");
    REQUIRE(print.config().retract_lift.get_at(0) == Approx(0.6));

    PrintingGroup &group = print.mutable_printing_plan().groups.front();
    PrintingExtrusion target = std::move(
        group.layers.front().tool_groups.front().extrusions.back());
    group.layers.front().tool_groups.front().extrusions.pop_back();
    group.layers.emplace_back();
    group.layers.back().print_z = scale_i(0.3);
    group.layers.emplace_back();
    PrintingLayerGroup &target_layer = group.layers.back();
    target_layer.print_z = scale_i(0.4);
    target_layer.tool_groups.emplace_back();
    target_layer.tool_groups.back().extruder_id = 0;
    target_layer.tool_groups.back().extrusions.push_back(std::move(target));

    run_layer_plugins(print, {
        TRANSITION_SCOPE_PLUGIN, RETRACTION_PLUGIN, ENTRY_STATE_PLUGIN,
        TRAVEL_PLUGIN, LIFT_PLUGIN
    });

    PrintingToolGroup &target_tool = target_layer.tool_groups.front();
    OrderedExtrusionScope scope(
        find_scope(entity_view(*target_tool.extrusions.front().root)),
        scope_property_key());
    const slic3r_api::MutableExtrusionEntity travel = scope.travel();
    REQUIRE(travel.child_count() == 2);
    // The original travel climbs from Z 0.2 to Z 0.4. Lift is added over that
    // existing profile, so the midpoint baseline is -0.1 relative to layer Z.
    CHECK(travel.child(0).z_offset(0) == -scale_i(0.2));
    CHECK(travel.child(0).z_offset(
              travel.child(0).point_count() - 1) == scale_i(0.5));
    CHECK(travel.child(1).z_offset(
              travel.child(1).point_count() - 1) == 0);
}

TEST_CASE("CreateRetractionLift materializes vertical lift and respects its threshold",
          "[plugins][layer-extrusion-edit][lift][travel]")
{
    Slic3r::Test::Plugins::ensure_plugin_test_runtime_initialized();

    SECTION("vertical profile") {
        Print print;
        Model model;
        prepare_lift_print(print, model, "0.8", "0", "0");
        run_layer_plugins(print, {
            TRANSITION_SCOPE_PLUGIN, RETRACTION_PLUGIN, ENTRY_STATE_PLUGIN,
            TRAVEL_PLUGIN, LIFT_PLUGIN
        });

        PrintingToolGroup &tool = print.mutable_printing_plan().groups.front()
            .layers.front().tool_groups.front();
        OrderedExtrusionScope scope(
            find_scope(entity_view(*tool.extrusions[1].root)),
            scope_property_key());
        const slic3r_api::MutableExtrusionEntity travel = scope.travel();
        REQUIRE(travel.segment_count() == 3);
        CHECK(travel.segment(0).point_a.x == travel.segment(0).point_b.x);
        CHECK(travel.segment(0).point_a.y == travel.segment(0).point_b.y);
        CHECK(travel.segment(0).z_offset_a == 0);
        CHECK(travel.segment(0).z_offset_b == scale_i(0.8));
        CHECK(travel.segment(2).point_a.x == travel.segment(2).point_b.x);
        CHECK(travel.segment(2).point_a.y == travel.segment(2).point_b.y);
        CHECK(travel.segment(2).z_offset_a == scale_i(0.8));
        CHECK(travel.segment(2).z_offset_b == 0);
    }

    SECTION("travel at threshold") {
        Print print;
        Model model;
        prepare_lift_print(print, model, "0.8", "0", "0", "10");
        run_layer_plugins(print, {
            TRANSITION_SCOPE_PLUGIN, RETRACTION_PLUGIN, ENTRY_STATE_PLUGIN,
            TRAVEL_PLUGIN, LIFT_PLUGIN
        });

        PrintingToolGroup &tool = print.mutable_printing_plan().groups.front()
            .layers.front().tool_groups.front();
        OrderedExtrusionScope scope(
            find_scope(entity_view(*tool.extrusions[1].root)),
            scope_property_key());
        const slic3r_api::MutableExtrusionEntity travel = scope.travel();
        REQUIRE(travel.segment_count() == 1);
        CHECK(travel.segment(0).z_offset_a == 0);
        CHECK(travel.segment(0).z_offset_b == 0);
    }
}

TEST_CASE("CreateRetractionLift raises only the retracting wipe prefix",
          "[plugins][layer-extrusion-edit][lift][wipe]")
{
    Slic3r::Test::Plugins::ensure_plugin_test_runtime_initialized();
    Print print;
    Model model;
    prepare_lift_print(print, model, "0", "0", "0", "0", true);

    run_layer_plugins(print, {
        TRANSITION_SCOPE_PLUGIN, RETRACTION_PLUGIN, WIPE_PLUGIN,
        ENTRY_STATE_PLUGIN, TRAVEL_PLUGIN, LIFT_PLUGIN
    });

    PrintingToolGroup &tool = print.mutable_printing_plan().groups.front()
        .layers.front().tool_groups.front();
    OrderedExtrusionScope source(
        find_scope(entity_view(*tool.extrusions[0].root)),
        scope_property_key());
    std::vector<LiftLeaf> leaves;
    collect_lift_leaves(source.after().readonly(), std::nullopt, leaves);

    const std::vector<LiftLeaf>::const_iterator retracting = std::find_if(
        leaves.begin(), leaves.end(), [](const LiftLeaf &leaf) {
            return RAW_EXTRUSION_ROLE_IS_WIPE(leaf.role) &&
                RAW_EXTRUSION_ROLE_IS_RETRACT(leaf.role);
        });
    const std::vector<LiftLeaf>::const_iterator mechanical = std::find_if(
        leaves.begin(), leaves.end(), [](const LiftLeaf &leaf) {
            return RAW_EXTRUSION_ROLE_IS_WIPE(leaf.role) &&
                !RAW_EXTRUSION_ROLE_IS_RETRACT(leaf.role);
        });
    REQUIRE(retracting != leaves.end());
    REQUIRE(mechanical != leaves.end());
    CHECK(retracting->entity.z_offset(0) == 0);
    CHECK(retracting->entity.z_offset(retracting->entity.point_count() - 1) ==
          scale_i(1.0));
    CHECK(mechanical->entity.z_offset(0) == 0);
    CHECK(mechanical->entity.z_offset(mechanical->entity.point_count() - 1) == 0);
}

} // namespace
