#include <catch2/catch.hpp>

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

#include "libslic3r/Api/host/ApiHostUtils.hpp"
#include "libslic3r/Api/host/Orchestrator.hpp"
#include "libslic3r/Api/host/Plugin.hpp"
#include "libslic3r/Api/plugin/c/slic3r_orchestrator.h"
#include "libslic3r/Api/plugin/cpp/OrchestratorViews.hpp"
#include "libslic3r/Config/ConfigDef.hpp"
#include "libslic3r/Steps/StepPipeline.hpp"

namespace {

using namespace Slic3r;

constexpr const char *ServiceStepName = "slic3r.test.generic_service";
constexpr const char *ServiceGroup = "slic3r.test.generic_service.provider";

struct TestPayload
{
    uint32_t expected_index = 0;
    bool setup_run_seen = false;
    bool run_seen = false;
};

enum class CallbackAction
{
    None,
    Cancel,
    ReportError
};

struct ServicePluginState
{
    const char *id = nullptr;
    slicing_step_t step = STEP_NONE;
    int32_t priority = 0;
    CallbackAction setup_action = CallbackAction::None;
    CallbackAction setup_run_action = CallbackAction::None;
    CallbackAction run_action = CallbackAction::None;
    bool allow_null_payload = false;
    uint32_t expected_run_count = 0;
    std::atomic<uint32_t> setup_run_count { 0 };
    std::atomic<uint32_t> run_count { 0 };
    std::atomic<bool> barrier_respected { true };
    std::atomic<bool> callback_contract_respected { true };
    std::atomic<bool> action_performed { false };
    storage_handle *storage = nullptr;
};

ServicePluginState g_service_low;
ServicePluginState g_service_high;
ServicePluginState g_private_numeric;
ServicePluginState g_private_numeric_alt;

void perform_callback_action(ServicePluginState &state,
                             const plugin_run_context *run_context,
                             CallbackAction action)
{
    if (action == CallbackAction::None ||
        state.action_performed.exchange(true, std::memory_order_relaxed))
        return;
    if (action == CallbackAction::Cancel) {
        Orchestrator::instance().request_plugin_cancel();
    } else if (run_context != nullptr && run_context->report_error != nullptr) {
        run_context->report_error(run_context->host_context, "intentional service error");
    }
}

const char *service_id(void *context)
{
    return static_cast<ServicePluginState *>(context)->id;
}

const char *service_name(void *context)
{
    return service_id(context);
}

const char *service_description(void *)
{
    return "Test provider for generic orchestrator execution.";
}

const char *service_group(void *)
{
    return ServiceGroup;
}

const char *service_group_label(void *)
{
    return "Generic service test provider";
}

const char *service_group_tooltip(void *)
{
    return "Choose the provider used by the generic execution API test.";
}

slicing_step_t service_step(void *context)
{
    return static_cast<ServicePluginState *>(context)->step;
}

const_strings_t service_dependencies(void *)
{
    return {};
}

int32_t service_priority(void *context)
{
    return static_cast<ServicePluginState *>(context)->priority;
}

int32_t service_used_config_keys(void *, raw_used_config_key *)
{
    return 0;
}

int32_t service_defined_config_keys(void *, const char **)
{
    return 0;
}

void service_initialize(void *, storage_handle *) {}

void service_setup(void *context, const plugin_run_context *run_context, uint32_t run_count)
{
    ServicePluginState &state = *static_cast<ServicePluginState *>(context);
    state.expected_run_count = run_count;
    state.setup_run_count.store(0, std::memory_order_relaxed);
    state.run_count.store(0, std::memory_order_relaxed);
    state.barrier_respected.store(true, std::memory_order_relaxed);
    state.callback_contract_respected.store(true, std::memory_order_relaxed);
    state.action_performed.store(false, std::memory_order_relaxed);
    state.storage = run_context != nullptr ? run_context->plugin_storage : nullptr;
    if (run_context == nullptr || run_context->data != nullptr || run_context->step != state.step)
        state.callback_contract_respected.store(false, std::memory_order_relaxed);
    perform_callback_action(state, run_context, state.setup_action);
}

void service_setup_run(void *context, const plugin_run_context *run_context)
{
    ServicePluginState &state = *static_cast<ServicePluginState *>(context);
    TestPayload *payload = run_context != nullptr ? static_cast<TestPayload *>(run_context->data) : nullptr;
    if (run_context == nullptr || run_context->host_context == nullptr ||
        (payload == nullptr && !state.allow_null_payload)) {
        state.callback_contract_respected.store(false, std::memory_order_relaxed);
        return;
    }
    const plugin_host_context *host = run_context->host_context;
    if (host->object_count != state.expected_run_count ||
        (payload != nullptr && host->object_idx != payload->expected_index) ||
        run_context->plugin_storage != state.storage)
        state.callback_contract_respected.store(false, std::memory_order_relaxed);
    if (payload != nullptr)
        payload->setup_run_seen = true;
    state.setup_run_count.fetch_add(1, std::memory_order_relaxed);

    perform_callback_action(state, run_context, state.setup_run_action);
}

void service_run(void *context, const plugin_run_context *run_context)
{
    ServicePluginState &state = *static_cast<ServicePluginState *>(context);
    TestPayload *payload = run_context != nullptr ? static_cast<TestPayload *>(run_context->data) : nullptr;
    if (payload == nullptr && !state.allow_null_payload) {
        state.callback_contract_respected.store(false, std::memory_order_relaxed);
        return;
    }
    if (state.setup_run_count.load(std::memory_order_relaxed) != state.expected_run_count)
        state.barrier_respected.store(false, std::memory_order_relaxed);
    if (payload != nullptr && !payload->setup_run_seen)
        state.callback_contract_respected.store(false, std::memory_order_relaxed);
    if (payload != nullptr)
        payload->run_seen = true;
    state.run_count.fetch_add(1, std::memory_order_relaxed);
    perform_callback_action(state, run_context, state.run_action);
}

const plugin_vtable *service_vtable()
{
    static const plugin_vtable table = {
        &service_id,
        &service_name,
        &service_description,
        &service_group,
        &service_group_label,
        &service_group_tooltip,
        &service_step,
        &service_dependencies,
        &service_priority,
        &service_used_config_keys,
        &service_defined_config_keys,
        &service_initialize,
        &service_setup,
        &service_setup_run,
        &service_run
    };
    return &table;
}

void register_service_plugin(Orchestrator &orchestrator, ServicePluginState &state)
{
    if (orchestrator.get_plugin(state.id) != nullptr)
        return;
    plugin_instance instance = {};
    instance.ctx = &state;
    instance.vt = service_vtable();
    REQUIRE(orchestrator.register_plugin(instance));
}

void ensure_service_plugins_registered()
{
    Orchestrator &orchestrator = Orchestrator::instance();
    const slicing_step_t step = orchestrator.register_step(ServiceStepName, STEP_LAYER_EXTRUSION_EDIT);
    REQUIRE(step >= SLICING_STEP_CUSTOM_BEGIN);
    REQUIRE(step <= SLICING_STEP_CUSTOM_END);

    g_service_low.id = "slic3r.test.generic_service.low";
    g_service_low.step = step;
    g_service_low.priority = -10;
    g_service_high.id = "slic3r.test.generic_service.high";
    g_service_high.step = step;
    g_service_high.priority = 10;
    g_private_numeric.id = "slic3r.test.private_numeric_service";
    g_private_numeric.step = static_cast<slicing_step_t>(12001);
    g_private_numeric.priority = 0;
    g_private_numeric_alt.id = "slic3r.test.private_numeric_service_alt";
    g_private_numeric_alt.step = g_private_numeric.step;
    g_private_numeric_alt.priority = 1;

    register_service_plugin(orchestrator, g_service_low);
    register_service_plugin(orchestrator, g_service_high);
    register_service_plugin(orchestrator, g_private_numeric);
    register_service_plugin(orchestrator, g_private_numeric_alt);
}

class ActivePluginGuard
{
public:
    ActivePluginGuard(Orchestrator &orchestrator, const std::vector<const char *> &plugin_ids)
        : m_orchestrator(orchestrator)
    {
        for (Plugin *plugin : m_orchestrator.active_plugins())
            m_previous.push_back(plugin);
        m_orchestrator.clear_active_plugins();
        for (const char *id : plugin_ids)
            REQUIRE(m_orchestrator.set_plugin_active(id, true));
    }

    ~ActivePluginGuard()
    {
        m_orchestrator.clear_active_plugins();
        for (Plugin *plugin : m_previous)
            m_orchestrator.set_plugin_active(plugin, true);
        m_orchestrator.reset_plugin_cancel();
        g_service_low.setup_action = CallbackAction::None;
        g_service_low.setup_run_action = CallbackAction::None;
        g_service_low.run_action = CallbackAction::None;
        g_service_low.allow_null_payload = false;
        g_service_high.setup_action = CallbackAction::None;
        g_service_high.setup_run_action = CallbackAction::None;
        g_service_high.run_action = CallbackAction::None;
    }

private:
    Orchestrator &m_orchestrator;
    std::vector<Plugin *> m_previous;
};

} // namespace

TEST_CASE("The orchestrator registers service steps by stable name",
          "[plugins][orchestrator][service-step]")
{
    ensure_service_plugins_registered();
    Orchestrator &orchestrator = Orchestrator::instance();
    orchestrator_handle *handle = reinterpret_cast<orchestrator_handle *>(&orchestrator);

    const slicing_step_t step = orchestrator_register_step(
        handle, ServiceStepName, STEP_LAYER_EXTRUSION_EDIT);
    CHECK(step == g_service_low.step);
    CHECK(std::string(orchestrator_step_name(handle, step)) == ServiceStepName);
    CHECK(orchestrator_register_step(handle, ServiceStepName, STEP_GCODE) == STEP_NONE);
    CHECK(orchestrator_register_step(handle, nullptr, STEP_GCODE) == STEP_NONE);
    CHECK(orchestrator_register_step(handle, "", STEP_GCODE) == STEP_NONE);
    CHECK(orchestrator_register_step(handle, "slic3r.test.invalid", STEP_NONE) == STEP_NONE);

    const slic3r_api::OrchestratorView view(handle);
    CHECK(view.register_step(ServiceStepName, STEP_LAYER_EXTRUSION_EDIT) == step);
    CHECK(view.step_name(step) == ServiceStepName);

    CHECK(orchestrator.get_plugin(g_private_numeric.id) != nullptr);
    CHECK(orchestrator_step_name(handle, g_private_numeric.step) == nullptr);
}

TEST_CASE("The orchestrator finds and selects generic service plugins",
          "[plugins][orchestrator][service-plugin]")
{
    ensure_service_plugins_registered();
    Orchestrator &orchestrator = Orchestrator::instance();
    orchestrator_handle *handle = reinterpret_cast<orchestrator_handle *>(&orchestrator);
    ActivePluginGuard active(orchestrator, {g_service_low.id, g_service_high.id});

    const orchestrator_plugin_handle *found = orchestrator_find_plugin(handle, g_service_low.id);
    REQUIRE(found != nullptr);
    CHECK(std::string(orchestrator_plugin_id(found)) == g_service_low.id);
    CHECK(orchestrator_plugin_step(found) == g_service_low.step);
    CHECK(orchestrator_find_plugin(handle, "slic3r.test.missing") == nullptr);

    const orchestrator_plugin_handle *fallback = orchestrator_select_plugin(
        handle, nullptr, g_service_low.step, ServiceGroup);
    REQUIRE(fallback != nullptr);
    CHECK(std::string(orchestrator_plugin_id(fallback)) == g_service_low.id);

    const slic3r_api::OrchestratorView view(handle);
    CHECK(view.select_plugin(nullptr, g_service_low.step, ServiceGroup).id() == g_service_low.id);

    DynamicConfig config;
    const std::string selector_key =
        "exclusive_group_slic3r_test_generic_service_slic3r_test_generic_service_provider_plugin";
    config.set_key_value(selector_key, new ConfigOptionInt(1));
    const orchestrator_plugin_handle *selected = orchestrator_select_plugin(
        handle, ApiHost::to_config_handle(&config), g_service_low.step, ServiceGroup);
    REQUIRE(selected != nullptr);
    CHECK(std::string(orchestrator_plugin_id(selected)) == g_service_high.id);

    const std::vector<Steps::StepExclusivePluginGroup> groups =
        Steps::active_exclusive_plugin_groups(orchestrator);
    const Steps::StepExclusivePluginGroup *service_group = nullptr;
    for (const Steps::StepExclusivePluginGroup &group : groups)
        if (group.group.step == g_service_low.step && group.group.group_id == ServiceGroup)
            service_group = &group;
    REQUIRE(service_group != nullptr);
    CHECK(service_group->group.option_key_storage == selector_key);
    CHECK(service_group->group.option_def.invalidates_step == STEP_LAYER_EXTRUSION_EDIT);

    {
        ActivePluginGuard private_active(
            orchestrator, {g_private_numeric.id, g_private_numeric_alt.id});
        const std::vector<Steps::StepExclusivePluginGroup> private_groups =
            Steps::active_exclusive_plugin_groups(orchestrator);
        const Steps::StepExclusivePluginGroup *private_group = nullptr;
        for (const Steps::StepExclusivePluginGroup &group : private_groups)
            if (group.group.step == g_private_numeric.step && group.group.group_id == ServiceGroup)
                private_group = &group;
        REQUIRE(private_group != nullptr);
        CHECK(private_group->group.option_def.invalidates_step == STEP_ANY);
    }
}

TEST_CASE("Generic plugin execution preserves its two-phase barrier",
          "[plugins][orchestrator][service-plugin]")
{
    ensure_service_plugins_registered();
    Orchestrator &orchestrator = Orchestrator::instance();
    orchestrator.reset_plugin_cancel();
    ActivePluginGuard active(orchestrator, {g_service_low.id});
    slic3r_api::OrchestratorView view(reinterpret_cast<orchestrator_handle *>(&orchestrator));
    const slic3r_api::PluginView plugin = view.find_plugin(g_service_low.id);
    REQUIRE(plugin.valid());

    std::vector<TestPayload> payloads(8);
    for (uint32_t idx = 0; idx < payloads.size(); ++idx)
        payloads[idx].expected_index = idx;

    CHECK(view.execute(plugin, nullptr, payloads) == RAW_PLUGIN_EXECUTION_SUCCESS);
    CHECK(g_service_low.setup_run_count.load(std::memory_order_relaxed) == payloads.size());
    CHECK(g_service_low.run_count.load(std::memory_order_relaxed) == payloads.size());
    CHECK(g_service_low.barrier_respected.load(std::memory_order_relaxed));
    CHECK(g_service_low.callback_contract_respected.load(std::memory_order_relaxed));
    CHECK(g_service_low.storage != nullptr);
    for (const TestPayload &payload : payloads) {
        CHECK(payload.setup_run_seen);
        CHECK(payload.run_seen);
    }

    TestPayload single;
    CHECK(view.execute(plugin, nullptr, single) == RAW_PLUGIN_EXECUTION_SUCCESS);
    CHECK(single.setup_run_seen);
    CHECK(single.run_seen);

    g_service_low.allow_null_payload = true;
    const std::vector<raw_plugin_run_payload> null_payloads(1, raw_plugin_run_payload { nullptr });
    CHECK(view.execute(plugin, nullptr, null_payloads) == RAW_PLUGIN_EXECUTION_SUCCESS);
    CHECK(g_service_low.callback_contract_respected.load(std::memory_order_relaxed));
    g_service_low.allow_null_payload = false;

    CHECK(view.execute_empty(plugin) == RAW_PLUGIN_EXECUTION_SUCCESS);
    CHECK(g_service_low.expected_run_count == 0);
}

TEST_CASE("Generic plugin execution reports inactive cancellation and plugin errors",
          "[plugins][orchestrator][service-plugin]")
{
    ensure_service_plugins_registered();
    Orchestrator &orchestrator = Orchestrator::instance();
    orchestrator.reset_plugin_cancel();
    orchestrator_handle *handle = reinterpret_cast<orchestrator_handle *>(&orchestrator);
    const orchestrator_plugin_handle *plugin = orchestrator_find_plugin(handle, g_service_low.id);
    REQUIRE(plugin != nullptr);
    const orchestrator_plugin_handle *invalid_plugin =
        reinterpret_cast<const orchestrator_plugin_handle *>(uintptr_t(1));
    CHECK(orchestrator_execute_plugin(handle, invalid_plugin, nullptr, nullptr, 0) ==
          RAW_PLUGIN_EXECUTION_INVALID_ARGUMENT);

    {
        ActivePluginGuard active(orchestrator, {});
        CHECK(orchestrator_execute_plugin(handle, plugin, nullptr, nullptr, 0) ==
              RAW_PLUGIN_EXECUTION_INACTIVE);
    }

    {
        ActivePluginGuard active(orchestrator, {g_service_low.id});
        orchestrator.request_plugin_cancel();
        CHECK(orchestrator_execute_plugin(handle, plugin, nullptr, nullptr, 0) ==
              RAW_PLUGIN_EXECUTION_CANCELLED);
        orchestrator.reset_plugin_cancel();

        CHECK(orchestrator_execute_plugin(handle, plugin, nullptr, nullptr, 1) ==
              RAW_PLUGIN_EXECUTION_INVALID_ARGUMENT);

        TestPayload payload;
        raw_plugin_run_payload raw_payload { &payload };
        g_service_low.setup_run_action = CallbackAction::ReportError;
        CHECK(orchestrator_execute_plugin(handle, plugin, nullptr, &raw_payload, 1) ==
              RAW_PLUGIN_EXECUTION_PLUGIN_ERROR);
        CHECK_FALSE(payload.run_seen);
        g_service_low.setup_run_action = CallbackAction::None;
        orchestrator.reset_plugin_cancel();

        g_service_low.setup_action = CallbackAction::Cancel;
        CHECK(orchestrator_execute_plugin(handle, plugin, nullptr, &raw_payload, 1) ==
              RAW_PLUGIN_EXECUTION_CANCELLED);
        CHECK(g_service_low.setup_run_count.load(std::memory_order_relaxed) == 0);
        g_service_low.setup_action = CallbackAction::None;
        orchestrator.reset_plugin_cancel();

        payload = TestPayload {};
        g_service_low.setup_run_action = CallbackAction::Cancel;
        CHECK(orchestrator_execute_plugin(handle, plugin, nullptr, &raw_payload, 1) ==
              RAW_PLUGIN_EXECUTION_CANCELLED);
        CHECK_FALSE(payload.run_seen);
        g_service_low.setup_run_action = CallbackAction::None;
        orchestrator.reset_plugin_cancel();

        payload = TestPayload {};
        g_service_low.run_action = CallbackAction::Cancel;
        CHECK(orchestrator_execute_plugin(handle, plugin, nullptr, &raw_payload, 1) ==
              RAW_PLUGIN_EXECUTION_CANCELLED);
        CHECK(payload.run_seen);
        g_service_low.run_action = CallbackAction::None;
        orchestrator.reset_plugin_cancel();

        payload = TestPayload {};
        g_service_low.setup_action = CallbackAction::ReportError;
        CHECK(orchestrator_execute_plugin(handle, plugin, nullptr, &raw_payload, 1) ==
              RAW_PLUGIN_EXECUTION_PLUGIN_ERROR);
        CHECK_FALSE(payload.setup_run_seen);
        g_service_low.setup_action = CallbackAction::None;
        orchestrator.reset_plugin_cancel();

        payload = TestPayload {};
        g_service_low.run_action = CallbackAction::ReportError;
        CHECK(orchestrator_execute_plugin(handle, plugin, nullptr, &raw_payload, 1) ==
              RAW_PLUGIN_EXECUTION_PLUGIN_ERROR);
        CHECK(payload.run_seen);
    }
}
