///|/ Copyright (c) SuperSlicer 2026 Durand Rémi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/
#ifndef slic3r_Api_plugin_cpp_OrchestratorViews_hpp_
#define slic3r_Api_plugin_cpp_OrchestratorViews_hpp_

#include <cassert>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

#include "libslic3r/Api/plugin/c/slic3r_orchestrator.h"

/*
OrchestratorViews.hpp
=====================

This header lets one plugin use another plugin as a service without exposing
the host's internal Plugin class. A consumer first registers the service step,
then asks OrchestratorView for either a provider with an exact id or the active
provider selected from an exclusive group. The returned PluginView is borrowed
and can be executed only through the same orchestrator.

The orchestrator owns the execution lifecycle and parallel barrier. Service
payloads remain private contracts between their caller and provider; this
generic layer transports their addresses but never interprets their contents.
*/

namespace slic3r_api {

class PluginView
{
public:
    // Construct an invalid view. Lookup methods use this value for "not found".
    PluginView() = default;

    // Return whether this view refers to a registered host plugin.
    bool valid() const { return m_plugin != nullptr; }

    // Return the stable plugin id, or an empty string for an invalid view.
    std::string id() const
    {
        const char *value = orchestrator_plugin_id(m_plugin);
        return value == nullptr ? std::string() : std::string(value);
    }

    // Return the step declared by the plugin, or STEP_NONE when invalid.
    slicing_step_t step() const { return orchestrator_plugin_step(m_plugin); }

    // Return the borrowed C handle for low-level interoperability.
    const orchestrator_plugin_handle *handle() const { return m_plugin; }

private:
    friend class OrchestratorView;

    // Only OrchestratorView creates valid views, preserving the association
    // between a borrowed plugin handle and its owning orchestrator.
    explicit PluginView(const orchestrator_plugin_handle *plugin) : m_plugin(plugin) {}

    const orchestrator_plugin_handle *m_plugin = nullptr;
};

class OrchestratorView
{
public:
    // Wrap a borrowed orchestrator supplied to plugin registration or stored
    // by PluginBase. The orchestrator must outlive this view and all PluginView
    // objects obtained from it.
    explicit OrchestratorView(orchestrator_handle *orchestrator) : m_orchestrator(orchestrator)
    {
        assert(m_orchestrator != nullptr);
    }

    // Accumulate a solidarity declaration without changing execution order.
    // Registration copies all strings; member plugins may be registered later.
    void register_activation_group(const std::string &id, const std::vector<std::string> &members) const
    {
        if (members.size() > std::numeric_limits<uint32_t>::max())
            throw std::invalid_argument("Too many activation group members.");
        std::vector<const char *> names;
        names.reserve(members.size());
        for (const std::string &member : members) names.push_back(member.c_str());
        if (!orchestrator_register_activation_group(m_orchestrator, id.c_str(),
                {names.data(), static_cast<uint32_t>(names.size())}))
            throw std::invalid_argument("Cannot register activation group '" + id +
                "': ID must be nonempty and each declaration needs at least two distinct nonempty members.");
    }

    // Register a namespaced service step. Failure is exceptional in C++
    // because a plugin cannot safely register or find its provider afterward.
    slicing_step_t register_step(const std::string &namespaced_name,
                                 slicing_step_t invalidates_step) const
    {
        const slicing_step_t step = orchestrator_register_step(
            m_orchestrator, namespaced_name.c_str(), invalidates_step);
        if (step == STEP_NONE)
            throw std::runtime_error("The plugin service step could not be registered.");
        return step;
    }

    // Return the stable name of a dynamic step. Built-in and unknown numeric
    // steps return an empty string because they have no runtime registry entry.
    std::string step_name(slicing_step_t step) const
    {
        const char *name = orchestrator_step_name(m_orchestrator, step);
        return name == nullptr ? std::string() : std::string(name);
    }

    // Find a registered plugin by exact id. The result may name an inactive
    // plugin; execute() reports INACTIVE instead of running it.
    PluginView find_plugin(const std::string &plugin_id) const
    {
        return PluginView(orchestrator_find_plugin(m_orchestrator, plugin_id.c_str()));
    }

    // Select the active provider from one exclusive group. config may be NULL;
    // in that case the first provider in normal priority order is returned.
    PluginView select_plugin(const config_handle *config,
                             slicing_step_t step,
                             const std::string &exclusive_group) const
    {
        return PluginView(orchestrator_select_plugin(
            m_orchestrator, config, step, exclusive_group.c_str()));
    }

    // Execute already prepared raw payload records. The caller keeps every
    // data pointer alive until both parallel phases have completed.
    raw_plugin_execution_status execute(const PluginView &plugin,
                                        const print_handle *print,
                                        const std::vector<raw_plugin_run_payload> &payloads) const
    {
        if (payloads.size() > std::numeric_limits<uint32_t>::max())
            return RAW_PLUGIN_EXECUTION_INVALID_ARGUMENT;
        return orchestrator_execute_plugin(m_orchestrator,
                                           plugin.handle(),
                                           print,
                                           payloads.empty() ? nullptr : payloads.data(),
                                           uint32_t(payloads.size()));
    }

    // Execute one typed payload without manually constructing the C record.
    template<class Payload>
    raw_plugin_execution_status execute(const PluginView &plugin,
                                        const print_handle *print,
                                        Payload &payload) const
    {
        const raw_plugin_run_payload raw_payload { &payload };
        return orchestrator_execute_plugin(m_orchestrator, plugin.handle(), print, &raw_payload, 1);
    }

    // Execute a contiguous vector of typed payloads. The vector must not be
    // resized while the call is active because workers borrow element addresses.
    template<class Payload>
    raw_plugin_execution_status execute(const PluginView &plugin,
                                        const print_handle *print,
                                        std::vector<Payload> &payloads) const
    {
        if (payloads.size() > std::numeric_limits<uint32_t>::max())
            return RAW_PLUGIN_EXECUTION_INVALID_ARGUMENT;
        std::vector<raw_plugin_run_payload> raw_payloads;
        raw_payloads.reserve(payloads.size());
        for (Payload &payload : payloads)
            raw_payloads.push_back(raw_plugin_run_payload { &payload });
        return orchestrator_execute_plugin(m_orchestrator,
                                           plugin.handle(),
                                           print,
                                           raw_payloads.empty() ? nullptr : raw_payloads.data(),
                                           uint32_t(raw_payloads.size()));
    }

    // Execute setup() with a zero run count and no per-run payloads.
    raw_plugin_execution_status execute_empty(const PluginView &plugin,
                                              const print_handle *print = nullptr) const
    {
        return orchestrator_execute_plugin(m_orchestrator, plugin.handle(), print, nullptr, 0);
    }

    // Return the borrowed C handle for APIs not wrapped by this class.
    orchestrator_handle *handle() const { return m_orchestrator; }

private:
    orchestrator_handle *m_orchestrator = nullptr;
};

} // namespace slic3r_api

#endif // slic3r_Api_plugin_cpp_OrchestratorViews_hpp_
