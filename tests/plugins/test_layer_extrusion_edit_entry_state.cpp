#include <catch2/catch.hpp>

#include <atomic>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <memory>
#include <stdexcept>
#include <vector>

#include "plugin_test_helpers.hpp"

#include "libslic3r/Api/host/Orchestrator.hpp"
#include "libslic3r/Api/host/Plugin.hpp"
#include "libslic3r/Api/plugin/c/steps/slic3r_step_layer_extrusion_edit.h"
#include "libslic3r/Api/plugin/cpp/PluginBase.hpp"
#include "libslic3r/Api/plugin/cpp/PrintingPlanViews.hpp"
#include "libslic3r/ExtrusionEntityCollection.hpp"
#include "libslic3r/ExtrusionProperty.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/Printing/PrintingPlan.hpp"
#include "libslic3r/Plugins/PrintingPlan/PrintingLayerEntryStateProperties.h"
#include "libslic3r/Steps/StepLayerExtrusionEdition.hpp"

/*
PrintingLayerGroup entry-state tests
====================================

These tests build the final ordered plan directly. That keeps the scenarios
focused on the boundary-state contract: the producer must scan once in plan
order, while every parallel run may write only the properties of its assigned
layer-group.
*/

namespace {
using namespace Slic3r;
using namespace Slic3r::Printing;
using slic3r_api::PluginBase;
using slic3r_api::PluginProperties;
using slic3r_api::PluginPropertyKey;
using slic3r_api::PrintingLayerEntryPositionProperty;
using slic3r_api::PrintingLayerEntryToolProperty;
using slic3r_api::printing_layer_entry_position_property_key;
using slic3r_api::printing_layer_entry_tool_property_key;

constexpr const char *ENTRY_STATE_PLUGIN = "layer_extrusion_edit.entry_state.default";
constexpr const char *ENTRY_STATE_CONSUMER = "tests.layer_extrusion_edit.entry_state.consumer";
const char *k_no_dependencies[] = { nullptr };

/* Restore the process-wide active plugin set after one host-runner scenario. */
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

/*
Probe scheduled after the producer. It deliberately reads only its current
layer-group, proving that the host's plugin barrier makes every snapshot visible
without following a predecessor pointer.
*/
class EntryStateConsumerProbe : public PluginBase
{
public:
    static EntryStateConsumerProbe &instance(orchestrator_handle *orchestrator)
    {
        static EntryStateConsumerProbe plugin(orchestrator);
        return plugin;
    }

    explicit EntryStateConsumerProbe(orchestrator_handle *orchestrator) :
        PluginBase(orchestrator),
        m_position_key(printing_layer_entry_position_property_key(orchestrator)),
        m_tool_key(printing_layer_entry_tool_property_key(orchestrator))
    {}

    void reset()
    {
        m_run_count = 0;
        m_missing_count = 0;
    }

    uint32_t run_count() const { return m_run_count.load(); }
    uint32_t missing_count() const { return m_missing_count.load(); }

private:
    const char *id_impl() const noexcept override { return ENTRY_STATE_CONSUMER; }
    const char *name_impl() const noexcept override { return "Entry-state consumer probe"; }
    const char *description_impl() const noexcept override
    {
        return "Checks layer entry-state publication barriers.";
    }
    const char *exclusive_group_impl() const noexcept override { return ""; }
    const char *exclusive_group_label_impl() const noexcept override { return ""; }
    const char *exclusive_group_tooltip_impl() const noexcept override { return ""; }
    slicing_step_t step_impl() const noexcept override { return STEP_LAYER_EXTRUSION_EDIT; }
    const char *const *dependencies_impl() const noexcept override { return k_no_dependencies; }
    int32_t priority_impl() const noexcept override { return -60; }

    void run_impl(const plugin_run_context *run_ctx) const override
    {
        const run_ctx_layer_extrusion_edition *ctx =
            plugin_ctx_as_layer_extrusion_edition(run_ctx);
        if (ctx == nullptr || ctx->layer_group == nullptr) {
            ++m_missing_count;
            return;
        }

        const slic3r_api::PrintingLayerGroup layer(ctx->layer_group);
        const PluginProperties properties = layer.properties();
        if (m_position_key.get(properties) == nullptr || m_tool_key.get(properties) == nullptr)
            ++m_missing_count;
        ++m_run_count;
    }

    PluginPropertyKey<PrintingLayerEntryPositionProperty> m_position_key;
    PluginPropertyKey<PrintingLayerEntryToolProperty> m_tool_key;
    mutable std::atomic_uint32_t m_run_count{0};
    mutable std::atomic_uint32_t m_missing_count{0};
};

/* Register the test-only consumer once in the process-wide orchestrator. */
EntryStateConsumerProbe &registered_consumer()
{
    Orchestrator &orchestrator = Orchestrator::instance();
    orchestrator_handle *handle = reinterpret_cast<orchestrator_handle *>(&orchestrator);
    EntryStateConsumerProbe &probe = EntryStateConsumerProbe::instance(handle);
    if (orchestrator.get_plugin(ENTRY_STATE_CONSUMER) == nullptr)
        REQUIRE(orchestrator.register_plugin(probe.c_instance()));
    return probe;
}

/* Create one geometric leaf with exact endpoint and per-point Z offsets. */
std::unique_ptr<ExtrusionPath> make_path(const Point &start,
                                        const Point &end,
                                        const coord_t start_z_offset,
                                        const coord_t end_z_offset,
                                        const ExtrusionRole role = ExtrusionRole::Perimeter)
{
    ArcPolyline polyline;
    polyline.append(start);
    polyline.append(end);
    polyline.set_z_offset(0, start_z_offset);
    polyline.set_z_offset(1, end_z_offset);
    return std::make_unique<ExtrusionPath>(
        polyline, ExtrusionAttributes(role, ExtrusionFlow(0.2, 0.4f, 0.2f)), nullptr, true);
}

/* Add one owned extrusion root to the ordered tool section. */
void append_extrusion(PrintingToolGroup &tool, std::unique_ptr<ExtrusionEntity> root)
{
    PrintingExtrusion extrusion;
    extrusion.root = std::move(root);
    extrusion.sregion_island_role = ExtrusionRole::Perimeter;
    tool.extrusions.push_back(std::move(extrusion));
}

/* Return a mutable plugin view over one native layer-group. */
slic3r_api::PrintingLayerGroup layer_view(Printing::PrintingLayerGroup &layer)
{
    return slic3r_api::PrintingLayerGroup(
        reinterpret_cast<printing_layer_group_handle *>(&layer));
}

/* Run the production entry-state plugin through the real parallel step host. */
void run_entry_state(Print &print, const bool with_consumer = false)
{
    ScopedActivePlugins active(with_consumer ?
        std::initializer_list<const char *>{ENTRY_STATE_PLUGIN, ENTRY_STATE_CONSUMER} :
        std::initializer_list<const char *>{ENTRY_STATE_PLUGIN});
    Orchestrator &orchestrator = Orchestrator::instance();
    orchestrator.reset_plugin_cancel();
    Steps::StepLayerExtrusionEdition::run_step(orchestrator, print);
    REQUIRE_FALSE(orchestrator.is_plugin_cancelled());
}

/* Resolve the two shared keys from the same orchestrator as the producer. */
PluginPropertyKey<PrintingLayerEntryPositionProperty> position_key()
{
    return printing_layer_entry_position_property_key(
        reinterpret_cast<orchestrator_handle *>(&Orchestrator::instance()));
}

PluginPropertyKey<PrintingLayerEntryToolProperty> tool_key()
{
    return printing_layer_entry_tool_property_key(
        reinterpret_cast<orchestrator_handle *>(&Orchestrator::instance()));
}

} // namespace

TEST_CASE("Layer entry-state properties use compatible dynamic contracts",
          "[plugins][layer-extrusion-edit][entry-state][properties]")
{
    Slic3r::Test::Plugins::ensure_plugin_test_runtime_initialized();

    Orchestrator &orchestrator = Orchestrator::instance();
    orchestrator_handle *handle = reinterpret_cast<orchestrator_handle *>(&orchestrator);
    Plugin *plugin = orchestrator.get_plugin(ENTRY_STATE_PLUGIN);
    REQUIRE(plugin != nullptr);
    CHECK(plugin->get_step() == STEP_LAYER_EXTRUSION_EDIT);
    CHECK(plugin->get_priority() == -70);
    CHECK(plugin->get_exclusive_group() == "layer_extrusion_edit.entry_state");
    CHECK(plugin->get_defined_config_keys().empty());
    CHECK(plugin->get_used_config_keys().empty());

    const PluginPropertyKey<PrintingLayerEntryPositionProperty> first_position =
        printing_layer_entry_position_property_key(handle);
    const PluginPropertyKey<PrintingLayerEntryPositionProperty> second_position =
        printing_layer_entry_position_property_key(handle);
    const PluginPropertyKey<PrintingLayerEntryToolProperty> first_tool =
        printing_layer_entry_tool_property_key(handle);
    CHECK(first_position.type() == second_position.type());
    CHECK(first_position.type() == register_printing_layer_entry_position_property(handle));
    CHECK(first_tool.type() == register_printing_layer_entry_tool_property(handle));

    struct IncompatiblePositionPayload
    {
        uint64_t value[8];
    };
    CHECK_THROWS_AS(
        PluginPropertyKey<IncompatiblePositionPayload>::register_dynamic(
            handle, PRINTING_LAYER_ENTRY_POSITION_PROPERTY_NAME),
        std::runtime_error);
}

TEST_CASE("Layer entry state follows geometry tools empty layers and groups",
          "[plugins][layer-extrusion-edit][entry-state]")
{
    Slic3r::Test::Plugins::ensure_plugin_test_runtime_initialized();

    Print print;
    PrintingPlan &plan = print.mutable_printing_plan();
    plan.groups.reserve(2);
    plan.groups.emplace_back();
    PrintingGroup &first_group = plan.groups.back();
    first_group.layers.reserve(4);

    first_group.layers.emplace_back();
    Printing::PrintingLayerGroup &first_layer = first_group.layers.back();
    first_layer.print_z = scale_i(0.2);
    first_layer.tool_groups.emplace_back();
    first_layer.tool_groups.back().extruder_id = 2;

    std::unique_ptr<ExtrusionEntityCollection> nested =
        std::make_unique<ExtrusionEntityCollection>(false, false);
    nested->append(make_path(
        Point(scale_i(1.), scale_i(1.)), Point(scale_i(2.), scale_i(3.)),
        scale_i(0.01), scale_i(0.06)));
    std::unique_ptr<ExtrusionEntityCollection> root =
        std::make_unique<ExtrusionEntityCollection>(false, false);
    root->add_property(ExtrusionPropertyZOffset(scale_i(0.04)));
    root->append(std::move(nested));
    ExtrusionEntity *first_root_address = root.get();
    append_extrusion(first_layer.tool_groups.back(), std::move(root));

    // A fully empty layer carries position and tool unchanged.
    first_group.layers.emplace_back();
    first_group.layers.back().print_z = scale_i(0.4);

    // An empty tool visit changes only the active tool.
    first_group.layers.emplace_back();
    Printing::PrintingLayerGroup &empty_tool_layer = first_group.layers.back();
    empty_tool_layer.print_z = scale_i(0.6);
    empty_tool_layer.tool_groups.emplace_back();
    empty_tool_layer.tool_groups.back().extruder_id = 5;

    // An explicit travel is geometric and therefore becomes the next entry position.
    first_group.layers.emplace_back();
    Printing::PrintingLayerGroup &travel_layer = first_group.layers.back();
    travel_layer.print_z = scale_i(0.6);
    travel_layer.tool_groups.emplace_back();
    travel_layer.tool_groups.back().extruder_id = 7;
    append_extrusion(
        travel_layer.tool_groups.back(),
        make_path(Point(scale_i(2.), scale_i(3.)), Point(scale_i(8.), scale_i(9.)),
                  0, scale_i(0.02), ExtrusionRole::Travel));

    // Group boundaries preserve the same machine continuity as file serialization.
    plan.groups.emplace_back();
    plan.groups.back().layers.emplace_back();
    Printing::PrintingLayerGroup &second_group_layer = plan.groups.back().layers.back();
    second_group_layer.print_z = scale_i(0.2);

    run_entry_state(print);

    const PluginPropertyKey<PrintingLayerEntryPositionProperty> positions = position_key();
    const PluginPropertyKey<PrintingLayerEntryToolProperty> tools = tool_key();

    const PrintingLayerEntryPositionProperty *first_position =
        positions.get(layer_view(first_layer).properties());
    const PrintingLayerEntryToolProperty *first_tool =
        tools.get(layer_view(first_layer).properties());
    REQUIRE(first_position != nullptr);
    REQUIRE(first_tool != nullptr);
    CHECK_FALSE(first_position->is_known());
    CHECK_FALSE(first_tool->has_active_tool());

    const PrintingLayerEntryPositionProperty *empty_position =
        positions.get(layer_view(first_group.layers[1]).properties());
    const PrintingLayerEntryToolProperty *empty_tool =
        tools.get(layer_view(first_group.layers[1]).properties());
    REQUIRE(empty_position != nullptr);
    REQUIRE(empty_tool != nullptr);
    CHECK(empty_position->is_known());
    CHECK(empty_position->x == scale_i(2.));
    CHECK(empty_position->y == scale_i(3.));
    CHECK(empty_position->z == scale_i(0.30));
    CHECK(empty_tool->extruder_id == 2);

    const PrintingLayerEntryPositionProperty *before_empty_tool =
        positions.get(layer_view(empty_tool_layer).properties());
    const PrintingLayerEntryToolProperty *before_empty_tool_id =
        tools.get(layer_view(empty_tool_layer).properties());
    REQUIRE(before_empty_tool != nullptr);
    REQUIRE(before_empty_tool_id != nullptr);
    CHECK(before_empty_tool->x == empty_position->x);
    CHECK(before_empty_tool->y == empty_position->y);
    CHECK(before_empty_tool->z == empty_position->z);
    CHECK(before_empty_tool_id->extruder_id == 2);

    const PrintingLayerEntryPositionProperty *before_travel =
        positions.get(layer_view(travel_layer).properties());
    const PrintingLayerEntryToolProperty *before_travel_tool =
        tools.get(layer_view(travel_layer).properties());
    REQUIRE(before_travel != nullptr);
    REQUIRE(before_travel_tool != nullptr);
    CHECK(before_travel->x == empty_position->x);
    CHECK(before_travel->y == empty_position->y);
    CHECK(before_travel->z == empty_position->z);
    CHECK(before_travel_tool->extruder_id == 5);

    const PrintingLayerEntryPositionProperty *next_group_position =
        positions.get(layer_view(second_group_layer).properties());
    const PrintingLayerEntryToolProperty *next_group_tool =
        tools.get(layer_view(second_group_layer).properties());
    REQUIRE(next_group_position != nullptr);
    REQUIRE(next_group_tool != nullptr);
    CHECK(next_group_position->x == scale_i(8.));
    CHECK(next_group_position->y == scale_i(9.));
    CHECK(next_group_position->z == scale_i(0.62));
    CHECK(next_group_tool->extruder_id == 7);

    // The producer annotates the plan only; source geometry and ownership stay intact.
    CHECK(first_layer.tool_groups.front().extrusions.front().root.get() == first_root_address);
    const slic3r_api::ExtrusionEntity root_view(
        reinterpret_cast<const extrusion_entity_handle *>(first_root_address));
    const c_point endpoint = root_view.back();
    CHECK(endpoint.x == scale_i(2.));
    CHECK(endpoint.y == scale_i(3.));
}

TEST_CASE("Layer entry state is replaced on rerun and visible to later plugins",
          "[plugins][layer-extrusion-edit][entry-state][parallel]")
{
    Slic3r::Test::Plugins::ensure_plugin_test_runtime_initialized();
    EntryStateConsumerProbe &consumer = registered_consumer();
    consumer.reset();

    Print print;
    PrintingPlan &plan = print.mutable_printing_plan();
    plan.groups.emplace_back();
    plan.groups.back().layers.resize(4);
    for (uint32_t layer_idx = 0; layer_idx < plan.groups.back().layers.size(); ++layer_idx)
        plan.groups.back().layers[layer_idx].print_z = scale_i(0.2 * double(layer_idx + 1));

    run_entry_state(print, true);
    CHECK(consumer.run_count() == 4);
    CHECK(consumer.missing_count() == 0);

    const PluginPropertyKey<PrintingLayerEntryPositionProperty> positions = position_key();
    const PluginPropertyKey<PrintingLayerEntryToolProperty> tools = tool_key();
    slic3r_api::PrintingLayerGroup first = layer_view(plan.groups.back().layers.front());
    PrintingLayerEntryPositionProperty &stale_position = positions.get_or_add(first.properties());
    PrintingLayerEntryToolProperty &stale_tool = tools.get_or_add(first.properties());
    stale_position.state = RAW_PRINTING_LAYER_ENTRY_POSITION_KNOWN;
    stale_position.x = 123;
    stale_position.y = 456;
    stale_position.z = 789;
    stale_tool.extruder_id = 12;

    consumer.reset();
    run_entry_state(print, true);
    CHECK(consumer.run_count() == 4);
    CHECK(consumer.missing_count() == 0);

    const PrintingLayerEntryPositionProperty *replaced_position = positions.get(first.properties());
    const PrintingLayerEntryToolProperty *replaced_tool = tools.get(first.properties());
    REQUIRE(replaced_position != nullptr);
    REQUIRE(replaced_tool != nullptr);
    CHECK_FALSE(replaced_position->is_known());
    CHECK_FALSE(replaced_tool->has_active_tool());
}
