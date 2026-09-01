///|/ Copyright (c) SuperSlicer 2026 Durand Remi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/

/*
Compact transition-scope tests
==============================

These tests exercise both the private named-scope helper and the production
provider through the real parallel STEP_LAYER_EXTRUSION_EDIT host. Small plans
make scope ownership, boundary pairing and conditional child layouts visible
without depending on later retraction, wipe or travel providers.
*/

#include <catch2/catch.hpp>

#include <cstdint>
#include <initializer_list>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "plugin_test_helpers.hpp"

#include "libslic3r/Api/host/Orchestrator.hpp"
#include "libslic3r/Api/host/Plugin.hpp"
#include "libslic3r/Api/plugin/cpp/PrintingEntityPropertyTraversal.hpp"
#include "libslic3r/ExtrusionEntity.hpp"
#include "libslic3r/ExtrusionProperty.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/Printing/PrintingPlan.hpp"
#include "libslic3r/Plugins/LayerExtrusionEdit/ExtrusionScopeHelpers.hpp"
#include "libslic3r/Plugins/PrintingPlan/PrintingExtrusionScopeProperty.h"
#include "libslic3r/Steps/StepLayerExtrusionEdition.hpp"

namespace {

using namespace Slic3r;
using namespace Slic3r::Printing;
using slic3r_api::LayerExtrusionEdit::ExtrusionScope::OrderedExtrusionScope;

constexpr const char *TRANSITION_SCOPE_PLUGIN =
    "layer_extrusion_edit.transition_scope.default";
constexpr const char *ENTRY_STATE_PLUGIN =
    "layer_extrusion_edit.entry_state.default";
constexpr const char *STRAIGHT_TRAVEL_PLUGIN =
    "layer_extrusion_edit.travel.default";

struct IncompatibleScopeProperty
{
    uint64_t flags = 0;
};

class ScopedActivePlugins
{
public:
    explicit ScopedActivePlugins(std::initializer_list<const char *> plugin_ids) :
        m_orchestrator(Orchestrator::instance())
    {
        for (Plugin *plugin : m_orchestrator.active_plugins())
            m_previous.push_back(plugin);
        m_orchestrator.clear_active_plugins();
        for (const char *plugin_id : plugin_ids)
            REQUIRE(m_orchestrator.set_plugin_active(plugin_id, true));
    }

    ~ScopedActivePlugins()
    {
        m_orchestrator.clear_active_plugins();
        for (Plugin *plugin : m_previous)
            m_orchestrator.set_plugin_active(plugin, true);
    }

private:
    Orchestrator &m_orchestrator;
    std::vector<Plugin *> m_previous;
};

/* Return the shared runtime key registered by the production provider. */
slic3r_api::PluginPropertyKey<slic3r_api::PrintingExtrusionScopeProperty>
scope_property_key();

/* Build one straight printable leaf with optional process modifiers. */
std::unique_ptr<ExtrusionPath> make_path(
    Point start,
    Point end,
    ExtrusionRole role = ExtrusionRole::Perimeter,
    bool enforce_retraction = false,
    bool toolchange_retraction = false);

/* Build a parent whose children remain in their supplied execution order. */
ExtrusionEntityUPtr make_parent(ExtrusionEntityUPtrs children,
                               bool sortable = false,
                               bool reversible = false);

/* Append one independently owned extrusion tree to a final tool visit. */
PrintingExtrusion &append_extrusion(PrintingToolGroup &tool,
                                    ExtrusionEntityUPtr root,
                                    uint16_t object_instance_idx = 0);

/* Append one layer and tool visit to the selected PrintingGroup. */
PrintingLayerGroup &append_layer(PrintingGroup &group,
                                 coord_t print_z,
                                 uint16_t extruder_id);

/* Run only transition preparation, optionally for a second idempotence pass. */
void run_transition_scopes(Print &print, uint32_t run_count = 1);

/* Wrap a host-owned core root in the mutable plugin API. */
slic3r_api::MutableExtrusionEntity entity_view(ExtrusionEntity &entity);

/* Read and require the compact marker attached to one core entity. */
const slic3r_api::PrintingExtrusionScopeProperty &scope_property(
    const ExtrusionEntity &entity);

/* Count marked roots recursively; scopes themselves are never nested. */
uint32_t scope_count(const ExtrusionEntity &entity);

/* Count explicit Travel leaves recursively after the current travel provider. */
uint32_t travel_count(const ExtrusionEntity &entity);

slic3r_api::PluginPropertyKey<slic3r_api::PrintingExtrusionScopeProperty>
scope_property_key()
{
    return slic3r_api::printing_extrusion_scope_property_key(
        reinterpret_cast<orchestrator_handle *>(&Orchestrator::instance()));
}

std::unique_ptr<ExtrusionPath> make_path(
    const Point start,
    const Point end,
    const ExtrusionRole role,
    const bool enforce_retraction,
    const bool toolchange_retraction)
{
    ArcPolyline polyline;
    polyline.append(start);
    polyline.append(end);
    std::unique_ptr<ExtrusionPath> path = std::make_unique<ExtrusionPath>(
        polyline,
        ExtrusionAttributes(
            role,
            ExtrusionFlow(role == ExtrusionRole::Travel ? 0.0 : 0.2, 0.4f, 0.2f)),
        nullptr,
        true);
    if (enforce_retraction || toolchange_retraction) {
        ExtrusionPropertyModifier modifier;
        modifier.set_enforce_retraction(enforce_retraction);
        modifier.set_toolchange_retraction(toolchange_retraction);
        path->add_property(modifier);
    }
    return path;
}

ExtrusionEntityUPtr make_parent(ExtrusionEntityUPtrs children,
                               const bool sortable,
                               const bool reversible)
{
    return std::make_unique<ExtrusionEntity>(
        std::move(children), sortable, reversible, !sortable);
}

PrintingExtrusion &append_extrusion(PrintingToolGroup &tool,
                                    ExtrusionEntityUPtr root,
                                    const uint16_t object_instance_idx)
{
    PrintingExtrusion extrusion;
    extrusion.root = std::move(root);
    extrusion.object_instance_idx = object_instance_idx;
    extrusion.sregion_island_role = ExtrusionRole::Perimeter;
    tool.extrusions.push_back(std::move(extrusion));
    return tool.extrusions.back();
}

PrintingLayerGroup &append_layer(PrintingGroup &group,
                                 const coord_t print_z,
                                 const uint16_t extruder_id)
{
    group.layers.emplace_back();
    PrintingLayerGroup &layer = group.layers.back();
    layer.print_z = print_z;
    layer.tool_groups.emplace_back();
    layer.tool_groups.back().extruder_id = extruder_id;
    return layer;
}

void run_transition_scopes(Print &print, const uint32_t run_count)
{
    ScopedActivePlugins active({TRANSITION_SCOPE_PLUGIN});
    Orchestrator &orchestrator = Orchestrator::instance();
    for (uint32_t run_idx = 0; run_idx < run_count; ++run_idx) {
        orchestrator.reset_plugin_cancel();
        Steps::StepLayerExtrusionEdition::run_step(orchestrator, print);
        REQUIRE_FALSE(orchestrator.is_plugin_cancelled());
    }
}

slic3r_api::MutableExtrusionEntity entity_view(ExtrusionEntity &entity)
{
    return slic3r_api::MutableExtrusionEntity(
        reinterpret_cast<extrusion_entity_handle *>(&entity));
}

const slic3r_api::PrintingExtrusionScopeProperty &scope_property(
    const ExtrusionEntity &entity)
{
    const slic3r_api::PrintingExtrusionScopeProperty *property =
        entity_view(const_cast<ExtrusionEntity &>(entity)).get(scope_property_key());
    REQUIRE(property != nullptr);
    return *property;
}

uint32_t scope_count(const ExtrusionEntity &entity)
{
    uint32_t count = entity_view(
        const_cast<ExtrusionEntity &>(entity)).get(scope_property_key()) != nullptr ? 1u : 0u;
    for (size_t child_idx = 0; child_idx < entity.child_count(); ++child_idx)
        count += scope_count(entity.child(child_idx));
    return count;
}

uint32_t travel_count(const ExtrusionEntity &entity)
{
    uint32_t count = 0;
    const ExtrusionAttributes *attributes =
        entity.get_property<ExtrusionAttributes>();
    if (attributes != nullptr && attributes->extrusion_role() == ExtrusionRole::Travel)
        ++count;
    for (size_t child_idx = 0; child_idx < entity.child_count(); ++child_idx)
        count += travel_count(entity.child(child_idx));
    return count;
}

} // namespace

TEST_CASE("Compact extrusion scope property uses one private runtime contract",
          "[plugins][layer-extrusion-edit][transition-scope][properties]")
{
    Slic3r::Test::Plugins::ensure_plugin_test_runtime_initialized();
    const slic3r_api::PluginPropertyKey<slic3r_api::PrintingExtrusionScopeProperty>
        first = scope_property_key();
    const slic3r_api::PluginPropertyKey<slic3r_api::PrintingExtrusionScopeProperty>
        second = scope_property_key();
    CHECK(first.type() == second.type());
    CHECK(first.type() >= SLIC3R_PROPERTY_TYPE_CUSTOM_BEGIN);
    CHECK_THROWS_AS(
        slic3r_api::PluginPropertyKey<IncompatibleScopeProperty>::register_dynamic(
            reinterpret_cast<orchestrator_handle *>(&Orchestrator::instance()),
            PRINTING_EXTRUSION_SCOPE_PROPERTY_NAME),
        std::runtime_error);
}

TEST_CASE("Ordered extrusion scope exposes its four compact layouts",
          "[plugins][layer-extrusion-edit][transition-scope][scope-helper]")
{
    Slic3r::Test::Plugins::ensure_plugin_test_runtime_initialized();
    const slic3r_api::PluginPropertyKey<slic3r_api::PrintingExtrusionScopeProperty>
        key = scope_property_key();

    const uint8_t shapes[] = {
        0,
        slic3r_api::PRINTING_EXTRUSION_SCOPE_OUTGOING_TRANSITION,
        slic3r_api::PRINTING_EXTRUSION_SCOPE_INCOMING_TRANSITION,
        uint8_t(slic3r_api::PRINTING_EXTRUSION_SCOPE_INCOMING_TRANSITION |
                slic3r_api::PRINTING_EXTRUSION_SCOPE_OUTGOING_TRANSITION)
    };
    const uint32_t child_counts[] = {0, 2, 3, 4};

    for (uint32_t shape_idx = 0; shape_idx < 4; ++shape_idx) {
        std::unique_ptr<ExtrusionPath> root = make_path(
            Point(0, 0), Point(scale_i(1.), 0));
        ExtrusionEntity *stable_root = root.get();
        slic3r_api::MutableExtrusionEntity view = entity_view(*root);
        OrderedExtrusionScope scope =
            slic3r_api::LayerExtrusionEdit::ExtrusionScope::ensure_scope(
                view, key, shapes[shape_idx]);

        CHECK(scope.root().handle() == reinterpret_cast<extrusion_entity_handle *>(stable_root));
        CHECK(scope.root().child_count() == child_counts[shape_idx]);
        CHECK(scope.has_incoming_transition() == (shape_idx >= 2));
        CHECK(scope.has_outgoing_transition() == (shape_idx == 1 || shape_idx == 3));
        CHECK(scope.travel().valid() == scope.has_incoming_transition());
        CHECK(scope.before().valid() == scope.has_incoming_transition());
        CHECK(scope.after().valid() == scope.has_outgoing_transition());
        CHECK(scope.content().valid());
        if (shape_idx == 0)
            CHECK(scope.content().handle() == scope.root().handle());
        else
            CHECK(scope.content().handle() != scope.root().handle());
        CHECK_FALSE(scope.root().readonly().sortable());
        CHECK_FALSE(scope.root().readonly().reversible());

        OrderedExtrusionScope repeated =
            slic3r_api::LayerExtrusionEdit::ExtrusionScope::ensure_scope(
                view, key, shapes[shape_idx]);
        CHECK(repeated.root().child_count() == child_counts[shape_idx]);
    }
}

TEST_CASE("Phased scopes move direct properties and resources with content",
          "[plugins][layer-extrusion-edit][transition-scope][scope-helper]")
{
    Slic3r::Test::Plugins::ensure_plugin_test_runtime_initialized();
    std::unique_ptr<ExtrusionPath> root = make_path(
        Point(0, 0), Point(scale_i(1.), 0));
    slic3r_api::MutableExtrusionEntity view = entity_view(*root);
    slic3r_api::EPropertyCustomGcode &custom = view.custom_gcode("M117 kept with content");
    const extrusion_data_id text_id = custom.text_id;

    OrderedExtrusionScope scope =
        slic3r_api::LayerExtrusionEdit::ExtrusionScope::ensure_scope(
            view, scope_property_key(),
            uint8_t(slic3r_api::PRINTING_EXTRUSION_SCOPE_INCOMING_TRANSITION |
                    slic3r_api::PRINTING_EXTRUSION_SCOPE_OUTGOING_TRANSITION));

    CHECK(scope.root().get(slic3r_api::EPropertyCustomGcode::key) == nullptr);
    const slic3r_api::EPropertyCustomGcode *preserved =
        scope.content().get(slic3r_api::EPropertyCustomGcode::key);
    REQUIRE(preserved != nullptr);
    CHECK(preserved->text_id == text_id);
    CHECK(scope.content().stored_string(text_id) == "M117 kept with content");
    CHECK(scope.content().segment_count() == 1);
    CHECK(scope.travel().empty());
    CHECK(scope.before().empty());
    CHECK(scope.after().empty());
}

TEST_CASE("Ordered extrusion scopes reject contradictory markers",
          "[plugins][layer-extrusion-edit][transition-scope][scope-helper]")
{
    Slic3r::Test::Plugins::ensure_plugin_test_runtime_initialized();
    const slic3r_api::PluginPropertyKey<slic3r_api::PrintingExtrusionScopeProperty>
        key = scope_property_key();

    std::unique_ptr<ExtrusionPath> malformed = make_path(
        Point(0, 0), Point(scale_i(1.), 0));
    slic3r_api::MutableExtrusionEntity malformed_view = entity_view(*malformed);
    malformed_view.disable_sort().disable_reverse();
    malformed_view.get_or_add(key).flags =
        slic3r_api::PRINTING_EXTRUSION_SCOPE_OUTGOING_TRANSITION;
    CHECK_THROWS_WITH(
        slic3r_api::LayerExtrusionEdit::ExtrusionScope::is_scope(
            malformed_view.readonly(), key),
        "An extrusion scope shape contradicts its transition flags.");

    std::unique_ptr<ExtrusionPath> valid = make_path(
        Point(0, 0), Point(scale_i(1.), 0));
    slic3r_api::MutableExtrusionEntity valid_view = entity_view(*valid);
    slic3r_api::LayerExtrusionEdit::ExtrusionScope::ensure_scope(
        valid_view, key, 0);
    CHECK_THROWS_WITH(
        slic3r_api::LayerExtrusionEdit::ExtrusionScope::ensure_scope(
            valid_view, key,
            slic3r_api::PRINTING_EXTRUSION_SCOPE_OUTGOING_TRANSITION),
        "An existing extrusion scope has different transition flags.");

    std::unique_ptr<ExtrusionPath> invalid_flags = make_path(
        Point(0, 0), Point(scale_i(1.), 0));
    CHECK_THROWS_AS(
        slic3r_api::LayerExtrusionEdit::ExtrusionScope::ensure_scope(
            entity_view(*invalid_flags), key,
            slic3r_api::PRINTING_EXTRUSION_SCOPE_INCOMING_TRAVEL_MATERIALIZED),
        std::invalid_argument);
}

TEST_CASE("Transition scope provider exposes its pipeline contract",
          "[plugins][layer-extrusion-edit][transition-scope]")
{
    Slic3r::Test::Plugins::ensure_plugin_test_runtime_initialized();
    Plugin *plugin = Orchestrator::instance().get_plugin(TRANSITION_SCOPE_PLUGIN);
    REQUIRE(plugin != nullptr);
    CHECK(plugin->get_step() == STEP_LAYER_EXTRUSION_EDIT);
    CHECK(plugin->get_priority() == -100);
    CHECK(plugin->get_exclusive_group() ==
          "layer_extrusion_edit.transition_scope");
    CHECK(plugin->get_dependencies().empty());
    CHECK(plugin->get_used_config_keys().empty());
}

TEST_CASE("Contiguous ownership scopes have no artificial phases",
          "[plugins][layer-extrusion-edit][transition-scope]")
{
    Slic3r::Test::Plugins::ensure_plugin_test_runtime_initialized();
    Print print;
    PrintingPlan &plan = print.mutable_printing_plan();
    plan.groups.emplace_back();
    PrintingLayerGroup &layer = append_layer(
        plan.groups.back(), scale_i(0.2), 0);
    PrintingToolGroup &tool = layer.tool_groups.back();
    ExtrusionEntity *first = append_extrusion(tool, make_path(
        Point(0, 0), Point(scale_i(1.), 0))).root.get();
    ExtrusionEntity *second = append_extrusion(tool, make_path(
        Point(scale_i(1.), 0), Point(scale_i(2.), 0))).root.get();

    run_transition_scopes(print);

    REQUIRE(first->child_count() == 0);
    REQUIRE(second->child_count() == 0);
    const slic3r_api::PrintingExtrusionScopeProperty &first_property =
        scope_property(*first);
    const slic3r_api::PrintingExtrusionScopeProperty &second_property =
        scope_property(*second);
    CHECK(slic3r_api::printing_extrusion_scope_has_flag(
        first_property, slic3r_api::PRINTING_EXTRUSION_SCOPE_START));
    CHECK_FALSE(slic3r_api::printing_extrusion_scope_has_flag(
        first_property,
        slic3r_api::PRINTING_EXTRUSION_SCOPE_OUTGOING_TRANSITION));
    CHECK_FALSE(slic3r_api::printing_extrusion_scope_has_flag(
        second_property,
        slic3r_api::PRINTING_EXTRUSION_SCOPE_INCOMING_TRANSITION));
    CHECK(slic3r_api::printing_extrusion_scope_has_flag(
        second_property, slic3r_api::PRINTING_EXTRUSION_SCOPE_TERMINAL));
}

TEST_CASE("Transition scope gaps use the scaled epsilon boundary",
          "[plugins][layer-extrusion-edit][transition-scope]")
{
    Slic3r::Test::Plugins::ensure_plugin_test_runtime_initialized();

    const coord_t gaps[] = {
        coord_t(SCALED_EPSILON - 1),
        coord_t(SCALED_EPSILON),
        coord_t(SCALED_EPSILON + 1)
    };
    for (uint32_t gap_idx = 0; gap_idx < 3; ++gap_idx) {
        Print print;
        PrintingPlan &plan = print.mutable_printing_plan();
        plan.groups.emplace_back();
        PrintingLayerGroup &layer = append_layer(
            plan.groups.back(), scale_i(0.2), 0);
        PrintingToolGroup &tool = layer.tool_groups.back();
        ExtrusionEntity *first = append_extrusion(tool, make_path(
            Point(0, 0), Point(scale_i(1.), 0))).root.get();
        ExtrusionEntity *second = append_extrusion(tool, make_path(
            Point(scale_i(1.) + gaps[gap_idx], 0), Point(scale_i(2.), 0))).root.get();

        run_transition_scopes(print);

        const bool transition = gap_idx != 0;
        CHECK(first->child_count() == (transition ? 2 : 0));
        CHECK(second->child_count() == (transition ? 3 : 0));
        CHECK(slic3r_api::printing_extrusion_scope_has_flag(
            scope_property(*first),
            slic3r_api::PRINTING_EXTRUSION_SCOPE_OUTGOING_TRANSITION) == transition);
        CHECK(slic3r_api::printing_extrusion_scope_has_flag(
            scope_property(*second),
            slic3r_api::PRINTING_EXTRUSION_SCOPE_INCOMING_TRANSITION) == transition);
    }
}

TEST_CASE("Transition scopes select the highest eligible continuous root",
          "[plugins][layer-extrusion-edit][transition-scope]")
{
    Slic3r::Test::Plugins::ensure_plugin_test_runtime_initialized();
    Print print;
    PrintingPlan &plan = print.mutable_printing_plan();
    plan.groups.emplace_back();
    PrintingLayerGroup &layer = append_layer(
        plan.groups.back(), scale_i(0.2), 0);
    PrintingToolGroup &tool = layer.tool_groups.back();

    ExtrusionEntityUPtrs continuous_children;
    continuous_children.emplace_back(make_path(
        Point(0, 0), Point(scale_i(1.), 0)));
    continuous_children.emplace_back(make_path(
        Point(scale_i(1.), 0), Point(scale_i(2.), 0)));
    ExtrusionEntity *continuous = append_extrusion(
        tool, make_parent(std::move(continuous_children))).root.get();

    ExtrusionEntityUPtrs sortable_children;
    sortable_children.emplace_back(make_path(
        Point(scale_i(2.), 0), Point(scale_i(3.), 0)));
    sortable_children.emplace_back(make_path(
        Point(scale_i(3.), 0), Point(scale_i(4.), 0)));
    ExtrusionEntity *sortable = append_extrusion(
        tool, make_parent(std::move(sortable_children), true, true)).root.get();

    run_transition_scopes(print);

    CHECK(entity_view(*continuous).get(scope_property_key()) != nullptr);
    CHECK(entity_view(continuous->child(0)).get(scope_property_key()) == nullptr);
    CHECK(entity_view(continuous->child(1)).get(scope_property_key()) == nullptr);
    CHECK(entity_view(*sortable).get(scope_property_key()) == nullptr);
    CHECK(entity_view(sortable->child(0)).get(scope_property_key()) != nullptr);
    CHECK(entity_view(sortable->child(1)).get(scope_property_key()) != nullptr);
    CHECK(scope_count(*continuous) == 1);
    CHECK(scope_count(*sortable) == 2);
}

TEST_CASE("Semantic boundaries split an otherwise continuous parent",
          "[plugins][layer-extrusion-edit][transition-scope]")
{
    Slic3r::Test::Plugins::ensure_plugin_test_runtime_initialized();
    Print print;
    PrintingPlan &plan = print.mutable_printing_plan();
    plan.groups.emplace_back();
    PrintingLayerGroup &layer = append_layer(
        plan.groups.back(), scale_i(0.2), 0);

    ExtrusionEntityUPtrs children;
    children.emplace_back(make_path(
        Point(0, 0), Point(scale_i(1.), 0)));
    children.emplace_back(make_path(
        Point(scale_i(1.), 0), Point(scale_i(2.), 0),
        ExtrusionRole::Perimeter, true));
    ExtrusionEntity *extrusion = append_extrusion(
        layer.tool_groups.back(), make_parent(std::move(children))).root.get();

    run_transition_scopes(print);

    CHECK(entity_view(*extrusion).get(scope_property_key()) == nullptr);
    REQUIRE(extrusion->child_count() == 2);
    CHECK(extrusion->child(0).child_count() == 2);
    CHECK(extrusion->child(1).child_count() == 3);
    CHECK(slic3r_api::printing_extrusion_scope_has_flag(
        scope_property(extrusion->child(0)),
        slic3r_api::PRINTING_EXTRUSION_SCOPE_OUTGOING_TRANSITION));
    CHECK(slic3r_api::printing_extrusion_scope_has_flag(
        scope_property(extrusion->child(1)),
        slic3r_api::PRINTING_EXTRUSION_SCOPE_INCOMING_TRANSITION));
}

TEST_CASE("Empty tool visits and existing travels create paired boundaries",
          "[plugins][layer-extrusion-edit][transition-scope]")
{
    Slic3r::Test::Plugins::ensure_plugin_test_runtime_initialized();

    SECTION("an empty transient tool visit is still a tool change") {
        Print print;
        PrintingPlan &plan = print.mutable_printing_plan();
        plan.groups.emplace_back();
        PrintingLayerGroup &layer = append_layer(
            plan.groups.back(), scale_i(0.2), 0);
        ExtrusionEntity *first = append_extrusion(
            layer.tool_groups.back(), make_path(
                Point(0, 0), Point(scale_i(1.), 0))).root.get();
        layer.tool_groups.emplace_back();
        layer.tool_groups.back().extruder_id = 1;
        layer.tool_groups.emplace_back();
        layer.tool_groups.back().extruder_id = 0;
        ExtrusionEntity *second = append_extrusion(
            layer.tool_groups.back(), make_path(
                Point(scale_i(1.), 0), Point(scale_i(2.), 0))).root.get();

        run_transition_scopes(print);

        CHECK(first->child_count() == 2);
        CHECK(second->child_count() == 3);
    }

    SECTION("a materialized Travel remains in place and marks both scopes") {
        Print print;
        PrintingPlan &plan = print.mutable_printing_plan();
        plan.groups.emplace_back();
        PrintingLayerGroup &layer = append_layer(
            plan.groups.back(), scale_i(0.2), 0);
        PrintingToolGroup &tool = layer.tool_groups.back();
        ExtrusionEntity *first = append_extrusion(tool, make_path(
            Point(0, 0), Point(scale_i(1.), 0))).root.get();
        ExtrusionEntity *travel = append_extrusion(tool, make_path(
            Point(scale_i(1.), 0), Point(scale_i(3.), 0),
            ExtrusionRole::Travel)).root.get();
        ExtrusionEntity *second = append_extrusion(tool, make_path(
            Point(scale_i(3.), 0), Point(scale_i(4.), 0))).root.get();

        run_transition_scopes(print);

        CHECK(entity_view(*travel).get(scope_property_key()) == nullptr);
        const slic3r_api::PrintingExtrusionScopeProperty &outgoing =
            scope_property(*first);
        const slic3r_api::PrintingExtrusionScopeProperty &incoming =
            scope_property(*second);
        CHECK(slic3r_api::printing_extrusion_scope_has_flag(
            outgoing,
            slic3r_api::PRINTING_EXTRUSION_SCOPE_OUTGOING_TRAVEL_MATERIALIZED));
        CHECK(slic3r_api::printing_extrusion_scope_has_flag(
            incoming,
            slic3r_api::PRINTING_EXTRUSION_SCOPE_INCOMING_TRAVEL_MATERIALIZED));
    }
}

TEST_CASE("Transition scopes pair boundaries across layers and PrintingGroups",
          "[plugins][layer-extrusion-edit][transition-scope][parallel]")
{
    Slic3r::Test::Plugins::ensure_plugin_test_runtime_initialized();
    Print print;
    PrintingPlan &plan = print.mutable_printing_plan();

    plan.groups.emplace_back();
    PrintingLayerGroup &first_layer = append_layer(
        plan.groups.back(), scale_i(0.2), 0);
    ExtrusionEntity *first = append_extrusion(
        first_layer.tool_groups.back(), make_path(
            Point(0, 0), Point(scale_i(1.), 0))).root.get();

    plan.groups.emplace_back();
    PrintingLayerGroup &second_layer = append_layer(
        plan.groups.back(), scale_i(0.4), 0);
    ExtrusionEntity *second = append_extrusion(
        second_layer.tool_groups.back(), make_path(
            Point(scale_i(2.), 0), Point(scale_i(3.), 0))).root.get();

    run_transition_scopes(print);

    CHECK(slic3r_api::printing_extrusion_scope_has_flag(
        scope_property(*first),
        slic3r_api::PRINTING_EXTRUSION_SCOPE_OUTGOING_TRANSITION));
    CHECK(slic3r_api::printing_extrusion_scope_has_flag(
        scope_property(*second),
        slic3r_api::PRINTING_EXTRUSION_SCOPE_INCOMING_TRANSITION));
    CHECK(slic3r_api::printing_extrusion_scope_has_flag(
        scope_property(*first),
        slic3r_api::PRINTING_EXTRUSION_SCOPE_START));
    CHECK(slic3r_api::printing_extrusion_scope_has_flag(
        scope_property(*second),
        slic3r_api::PRINTING_EXTRUSION_SCOPE_TERMINAL));
}

TEST_CASE("Transition scope preparation is idempotent and traversal skips phases",
          "[plugins][layer-extrusion-edit][transition-scope]")
{
    Slic3r::Test::Plugins::ensure_plugin_test_runtime_initialized();
    Print print;
    PrintingPlan &plan = print.mutable_printing_plan();
    plan.groups.emplace_back();
    PrintingLayerGroup &layer = append_layer(
        plan.groups.back(), scale_i(0.2), 0);
    PrintingToolGroup &tool = layer.tool_groups.back();
    ExtrusionEntity *first = append_extrusion(tool, make_path(
        Point(0, 0), Point(scale_i(1.), 0))).root.get();
    ExtrusionEntity *second = append_extrusion(tool, make_path(
        Point(scale_i(3.), 0), Point(scale_i(4.), 0))).root.get();

    run_transition_scopes(print, 2);

    CHECK(first->child_count() == 2);
    CHECK(second->child_count() == 3);
    CHECK(scope_count(*first) == 1);
    CHECK(scope_count(*second) == 1);

    const slic3r_api::PrintingLayerGroup layer_view(
        reinterpret_cast<printing_layer_group_handle *>(&layer));
    using Traversed =
        slic3r_api::PrintingEntity<slic3r_api::PrintingExtrusionScopeProperty>;
    uint32_t starts = 0;
    uint32_t pairs = 0;
    uint32_t ends = 0;
    slic3r_api::PrintingEntityPropertyTraversal<
        slic3r_api::PrintingExtrusionScopeProperty> traversal(
            scope_property_key(),
            [&starts, &pairs, &ends](Traversed *previous, Traversed *next) {
                if (previous == nullptr)
                    ++starts;
                else if (next == nullptr)
                    ++ends;
                else
                    ++pairs;
            },
            slic3r_api::MatchingEntityDescendants::Skip);
    traversal.process(layer_view);
    CHECK(starts == 1);
    CHECK(pairs == 1);
    CHECK(ends == 1);
}

TEST_CASE("Current straight travel remains compatible with compact scopes",
          "[plugins][layer-extrusion-edit][transition-scope][travel]")
{
    Slic3r::Test::Plugins::ensure_plugin_test_runtime_initialized();
    Print print;
    PrintingPlan &plan = print.mutable_printing_plan();
    plan.groups.emplace_back();
    PrintingLayerGroup &layer = append_layer(
        plan.groups.back(), scale_i(0.2), 0);
    PrintingToolGroup &tool = layer.tool_groups.back();
    ExtrusionEntity *first = append_extrusion(tool, make_path(
        Point(0, 0), Point(scale_i(1.), 0))).root.get();
    ExtrusionEntity *second = append_extrusion(tool, make_path(
        Point(scale_i(3.), 0), Point(scale_i(4.), 0))).root.get();

    ScopedActivePlugins active({
        TRANSITION_SCOPE_PLUGIN, ENTRY_STATE_PLUGIN, STRAIGHT_TRAVEL_PLUGIN});
    Orchestrator &orchestrator = Orchestrator::instance();
    orchestrator.reset_plugin_cancel();
    Steps::StepLayerExtrusionEdition::run_step(orchestrator, print);
    REQUIRE_FALSE(orchestrator.is_plugin_cancelled());

    CHECK(scope_count(*first) == 1);
    CHECK(scope_count(*second) == 1);
    CHECK(travel_count(*first) + travel_count(*second) == 1);

    // Re-running the complete chain validates the existing scopes and keeps
    // the final travel-provider flags instead of deriving them from scratch.
    Steps::StepLayerExtrusionEdition::run_step(orchestrator, print);
    REQUIRE_FALSE(orchestrator.is_plugin_cancelled());
    CHECK(scope_count(*first) == 1);
    CHECK(scope_count(*second) == 1);
    CHECK(travel_count(*first) + travel_count(*second) == 1);
}
