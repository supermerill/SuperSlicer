///|/ Copyright (c) SuperSlicer 2026 Durand Rémi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/
#ifndef slic3r_Api_plugin_cpp_PluginBase_hpp_
#define slic3r_Api_plugin_cpp_PluginBase_hpp_

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <string>
#include <vector>

#include "libslic3r/Api/plugin/c/slic3r_plugin.h"
#include "libslic3r/Api/plugin/cpp/PluginContext.hpp"

/*
PluginBase.hpp
==============

This file is the small C++ SDK layer used to write plugins without manually
building a C ABI vtable.

It contains two main helpers:

- PluginBase:
  Derive your C++ plugin class from PluginBase, implement the *_impl() virtual
  methods, then register `YourPlugin::instance().c_instance()` with the
  orchestrator. PluginBase converts your C++ class into the C ABI expected by
  the host and catches C++ exceptions before they cross the ABI boundary.

- PluginProgress:
  A thread-safe progress counter owned by PluginBase. Use it when one plugin is
  executed multiple times in parallel, usually once per print object. The host
  only displays the progress value it receives; this helper lets the plugin
  estimate its own total work during setup_run_impl() and report progress from
  run_impl().

Typical plugin structure:

    class MyPlugin : public slic3r_api::PluginBase
    {
    private:
        const char *id_impl() const noexcept override { return "my_plugin"; }
        slicing_step_t step_impl() const noexcept override { return STEP_POST_SLICING; }
        const char *const *dependencies_impl() const noexcept override { return k_deps; }
        int32_t priority_impl() const noexcept override { return 0; }

        const char *progress_message_format_impl() const noexcept override
        {
            return "My plugin: %u / %u";
        }

        void setup_run_impl(const plugin_run_context *ctx) const override
        {
            // Called once per future run, before any run_impl() starts.
            // Add the amount of work this run will do.
            progress().add_max(max_count_for_this_context);
        }

        void run_impl(const plugin_run_context *ctx) const override
        {
            for (...) {
                slic3r_api::throw_if_cancelled(ctx);
                // do one unit of work
                progress().increment();
            }
        }
    };

Execution order guaranteed by the host for a given step/plugin pair:

    setup_impl(run_count)        // once, resets PluginProgress
    setup_run_impl(ctx) x N      // may be parallel, but all finish before run
    run_impl(ctx) x N            // may be parallel

Only run_impl() should modify the slicer data tree. setup_run_impl() is meant
for preparation and work estimation.
*/

namespace slic3r_api {

// Thread-safe progress helper for C++ plugins.
//
// The helper counts "work units". During setup_run_impl(), call
// add_max() with the amount of work each run expects to do. During run_impl(),
// call increment()/increment_by() whenever work is completed. A progress update
// is sent to the host only when the visible percentage changes.
class PluginProgress
{
public:
    static constexpr uint32_t DefaultPhase = 0;
    static constexpr uint32_t MaxPhases = 16;

    // Reset all counters. PluginBase calls this automatically in setup()
    // (before setup_impl()), so normal plugins do not need to call it.
    // Only the host progress callback is stored; plugin_run_context itself is
    // not kept because run contexts are copied per worker thread.
    void reset(const plugin_run_context *ctx, uint32_t run_count, const char *message_format)
    {
        m_host_context = ctx != nullptr ? ctx->host_context : nullptr;
        m_report_progress = ctx != nullptr ? ctx->report_progress : nullptr;
        m_run_count.store(run_count, std::memory_order_relaxed);
        for (PhaseProgress &phase : m_phases) {
            phase.done.store(0, std::memory_order_relaxed);
            phase.max.store(0, std::memory_order_relaxed);
            phase.finished_runs.store(0, std::memory_order_relaxed);
            phase.active.store(0, std::memory_order_relaxed);
            phase.message_format = message_format != nullptr ? message_format : "Plugin progress: %u / %u";
        }
        m_phases[DefaultPhase].active.store(1, std::memory_order_relaxed);
        m_last_percent.store(-1, std::memory_order_relaxed);
        m_last_phase.store(-1, std::memory_order_relaxed);
    }

    // Configure the message used when a phase is the visible phase. The format
    // receives two integers: completed work and total work for that phase.
    void set_phase_format(uint32_t phase, const char *message_format)
    {
        if (phase >= MaxPhases)
            return;
        m_phases[phase].message_format = message_format != nullptr ? message_format : "Plugin progress: %u / %u";
        m_phases[phase].active.store(1, std::memory_order_relaxed);
    }

    // Add expected work units. This is atomic so setup_run_impl() may be called
    // in parallel for multiple objects.
    void add_max(uint32_t count)
    {
        add_max(DefaultPhase, count);
    }

    void add_max(uint32_t phase, uint32_t count)
    {
        if (phase >= MaxPhases)
            return;
        m_phases[phase].active.store(1, std::memory_order_relaxed);
        m_phases[phase].max.fetch_add(count, std::memory_order_relaxed);
    }

    // Mark one work unit as completed and maybe report progress.
    void increment()
    {
        increment(DefaultPhase);
    }

    void increment(uint32_t phase)
    {
        increment_by(phase, 1);
    }

    // Move progress by `count` work units and maybe report progress. `count`
    // may be negative if a plugin intentionally wants to move the bar back.
    void increment_by(int32_t count)
    {
        increment_by(DefaultPhase, count);
    }

    void increment_by(uint32_t phase, int32_t count)
    {
        if (phase >= MaxPhases)
            return;
        m_phases[phase].active.store(1, std::memory_order_relaxed);
        const int32_t done = add_done(phase, count);
        report_if_needed(phase, phase_message(phase, done).c_str());
    }

    // Mark this run as done with a phase. A phase is considered globally
    // finished only once every planned run called finish_run() for it.
    void finish_run(uint32_t phase = DefaultPhase)
    {
        if (phase >= MaxPhases)
            return;
        m_phases[phase].active.store(1, std::memory_order_relaxed);
        const uint32_t finished = m_phases[phase].finished_runs.fetch_add(1, std::memory_order_relaxed) + 1;
        const int32_t done = m_phases[phase].done.load(std::memory_order_relaxed);
        const uint32_t max = m_phases[phase].max.load(std::memory_order_relaxed);
        if (finished >= m_run_count.load(std::memory_order_relaxed) && done < int32_t(max))
            m_phases[phase].done.store(int32_t(max), std::memory_order_relaxed);
        report_if_needed(phase, phase_message(phase).c_str());
    }

    // Convenience wrapper around increment() with one-shot printf-style
    // formatting. Most plugins should prefer progress_message_format_impl()
    // and plain increment().
    template<class... Args>
    void incrementf(const char *format, Args... args)
    {
        increment_byf(DefaultPhase, 1, format, args...);
    }

    template<class... Args>
    void incrementf(uint32_t phase, const char *format, Args... args)
    {
        increment_byf(phase, 1, format, args...);
    }

    // Same as incrementf(), but moves multiple work units at once.
    template<class... Args>
    void increment_byf(int32_t count, const char *format, Args... args)
    {
        increment_byf(DefaultPhase, count, format, args...);
    }

    template<class... Args>
    void increment_byf(uint32_t phase, int32_t count, const char *format, Args... args)
    {
        if (phase >= MaxPhases)
            return;
        m_phases[phase].active.store(1, std::memory_order_relaxed);
        add_done(phase, count);
        report_if_needed(phase, format_message(format, args...).c_str());
    }

    int32_t done() const { return done(DefaultPhase); }
    int32_t done(uint32_t phase) const { return phase < MaxPhases ? m_phases[phase].done.load(std::memory_order_relaxed) : 0; }
    uint32_t max() const { return max(DefaultPhase); }
    uint32_t max(uint32_t phase) const { return phase < MaxPhases ? m_phases[phase].max.load(std::memory_order_relaxed) : 0; }
    uint32_t run_count() const { return m_run_count.load(std::memory_order_relaxed); }

private:
    struct PhaseProgress
    {
        std::atomic<int32_t> done { 0 };
        std::atomic<uint32_t> max { 0 };
        std::atomic<uint32_t> finished_runs { 0 };
        std::atomic<uint32_t> active { 0 };
        const char *message_format = "Plugin progress: %u / %u";
    };

    // Convert completed work units to a [0.0, 1.0] progress value and pass it
    // through the C ABI callback when the visible percentage changes.
    int32_t add_done(uint32_t phase, int32_t delta)
    {
        int32_t current = m_phases[phase].done.load(std::memory_order_relaxed);
        for (;;) {
            const int32_t next = current + delta;
            if (m_phases[phase].done.compare_exchange_weak(current,
                                                           next,
                                                           std::memory_order_relaxed,
                                                           std::memory_order_relaxed))
                return next;
        }
    }

    bool phase_finished(uint32_t phase) const
    {
        const PhaseProgress &progress = m_phases[phase];
        if (progress.active.load(std::memory_order_relaxed) == 0)
            return true;
        if (progress.finished_runs.load(std::memory_order_relaxed) < m_run_count.load(std::memory_order_relaxed))
            return false;
        return progress.done.load(std::memory_order_relaxed) >= int32_t(progress.max.load(std::memory_order_relaxed));
    }

    int32_t first_visible_phase() const
    {
        for (uint32_t phase = 0; phase < MaxPhases; ++phase) {
            if (!phase_finished(phase))
                return int32_t(phase);
        }
        return -1;
    }

    void report_if_needed(uint32_t updated_phase, const char *message)
    {
        if (m_report_progress == nullptr)
            return;

        const int32_t visible_phase = first_visible_phase();
        if (visible_phase < 0)
            return;

        const PhaseProgress &progress = m_phases[uint32_t(visible_phase)];
        const uint32_t max = progress.max.load(std::memory_order_relaxed);
        if (max == 0)
            return;

        const int32_t done = progress.done.load(std::memory_order_relaxed);
        const uint32_t clamped_done = uint32_t(std::clamp<int64_t>(done, 0, int64_t(max)));
        const int percent = int((uint64_t(clamped_done) * 100u) / max);
        const bool phase_changed = m_last_phase.exchange(visible_phase, std::memory_order_relaxed) != visible_phase;
        if (phase_changed)
            m_last_percent.store(-1, std::memory_order_relaxed);

        const int previous_percent = m_last_percent.exchange(percent, std::memory_order_relaxed);
        if (phase_changed || percent != previous_percent) {
            const std::string visible_message =
                updated_phase == uint32_t(visible_phase) ? std::string(message != nullptr ? message : "") :
                phase_message(uint32_t(visible_phase), done);
            m_report_progress(m_host_context, double(percent) / 100.0, visible_message.c_str());
        }
    }

    std::string phase_message(uint32_t phase) const
    {
        return phase_message(phase, m_phases[phase].done.load(std::memory_order_relaxed));
    }

    std::string phase_message(uint32_t phase, int32_t done) const
    {
        const PhaseProgress &progress = m_phases[phase];
        const uint32_t max = progress.max.load(std::memory_order_relaxed);
        const uint32_t clamped_done = uint32_t(std::clamp<int64_t>(done, 0, int64_t(max)));
        return format_message(progress.message_format, clamped_done, max);
    }

    template<class... Args>
    static std::string format_message(const char *format, Args... args)
    {
        if (format == nullptr)
            return {};

        const int needed = std::snprintf(nullptr, 0, format, args...);
        if (needed <= 0)
            return {};

        std::vector<char> buffer(size_t(needed) + 1);
        std::snprintf(buffer.data(), buffer.size(), format, args...);
        return std::string(buffer.data(), size_t(needed));
    }

    std::array<PhaseProgress, MaxPhases> m_phases;
    std::atomic<uint32_t> m_run_count { 0 };
    std::atomic<int> m_last_percent { -1 };
    std::atomic<int32_t> m_last_phase { -1 };
    plugin_host_context *m_host_context = nullptr;
    plugin_report_progress_fn m_report_progress = nullptr;
};

// Base class for C++ plugins exposed through the C ABI.
//
// A plugin author normally only interacts with the protected section:
// implement the required *_impl() methods, optionally implement setup_impl()
// and setup_run_impl(), and use progress() from run_impl().
class PluginBase
{
public:
    virtual ~PluginBase() = default;

    // Return the C ABI instance registered with orchestrator_register_plugin().
    // Built-in plugins usually expose this through a register_*_plugin()
    // function; external plugin DLLs usually call it from register_plugin().
    plugin_instance c_instance() const
    {
        plugin_instance instance = {};
        instance.ctx = const_cast<PluginBase *>(this);
        instance.vt = &vtable();
        return instance;
    }

protected:
    // Constructor: please give the orchestrator that register you so you can reuse it afterwards.
    PluginBase(orchestrator_handle *orch) : m_orchestrator(orch) {
        assert(m_orchestrator);
    }

    // Shared progress helper owned by this plugin instance.
    //
    // It is mutable internally so run_impl() can stay const while still
    // reporting progress from multiple worker threads.
    PluginProgress &progress() const { return m_progress; }

    // Stable plugin identifier. It must be unique among registered plugins.
    virtual const char *id_impl() const noexcept = 0;

    // Short user-facing plugin name. It is used in plugin selectors while
    // id_impl() remains the stable serialized value. Override when the id is
    // too technical for end users.
    virtual const char *name_impl() const noexcept { return id_impl(); }

    // Longer user-facing description. Dialogs may show this as helper text.
    virtual const char *description_impl() const noexcept { return ""; }

    // Exclusive group id. Plugins sharing the same group are mutually
    // exclusive: the host creates one selector and runs only the selected
    // active plugin. A plugin that has no alternatives should use its own id as
    // a singleton group; this makes it possible for a future alternative to
    // target the old plugin without modifying the old implementation.
    virtual const char *exclusive_group_impl() const noexcept { return id_impl(); }

    // Optional user-facing text for the selector created for an exclusive
    // group. The first active plugin in the group that returns non-empty text
    // provides the label/tooltip for the whole group.
    virtual const char *exclusive_group_label_impl() const noexcept { return ""; }
    virtual const char *exclusive_group_tooltip_impl() const noexcept { return ""; }

    // Pipeline step where the plugin runs, for example STEP_POST_SLICING.
    virtual slicing_step_t step_impl() const noexcept = 0;

    // Null-terminated dependency list. Return `{ nullptr }` when there are no
    // dependencies. PluginBase converts it to const_strings_t for the C ABI.
    virtual const char *const *dependencies_impl() const noexcept = 0;

    // Lower priority runs first inside a step.
    virtual int32_t priority_impl() const noexcept = 0;

    // Configuration options read or defined by this plugin. The default is
    // empty; every key returned by defined_config_keys() must also be present.
    // Return the required entry count when keys is nullptr, otherwise fill the
    // caller-provided array with borrowed key pointers and type expectations.
    virtual int32_t used_config_keys(raw_used_config_key *keys) const noexcept
    {
        (void)keys;
        return 0;
    }

    // Configuration option keys created by this plugin. The host uses this
    // declaration for early activation checks; orchestrator_create_option_def()
    // still performs the full definition compatibility validation. The host
    // obtains each defined key's type from used_config_keys().
    virtual int32_t defined_config_keys(const char **keys) const noexcept
    {
        (void)keys;
        return 0;
    }

    // Optional: message format used by PluginProgress when increment() reports
    // progress. It receives two unsigned integers: completed work and total
    // expected work. Override for clearer plugin-specific messages.
    virtual const char *progress_message_format_impl() const noexcept
    {
        return "Plugin progress: %u / %u";
    }
    
    // Optional: called once at startup.
    // You can ask the orchestrator to add settings definitions here.
    virtual void inilialize_impl(storage_handle *storage) const {};

    // Optional: called once before setup_run_impl()/run_impl().
    // PluginBase resets progress before calling this method.
    virtual void setup_impl(const plugin_run_context *, uint32_t) const {}

    // Optional: called once for each future run context, before any run_impl()
    // starts. Use it to call progress().add_max(...).
    virtual void setup_run_impl(const plugin_run_context *) const {}

    // Required: perform the plugin work for one run context. This method may be
    // called in parallel for different objects, so shared plugin state must be
    // protected or atomic.
    virtual void run_impl(const plugin_run_context *run_ctx) const = 0;

private:
    // The *_safe() wrappers are the C++/C ABI safety belt: exceptions are
    // converted to plugin errors instead of crossing function pointers.

    void initialize_safe(storage_handle *storage) const noexcept {
        try {
            inilialize_impl(storage);
        } catch (...) {
            assert(false);
            return;
        }
    }


    void setup_safe(const plugin_run_context *run_ctx, uint32_t run_count) const noexcept
    {
        try {
            m_progress.reset(run_ctx, run_count, progress_message_format_impl());
            setup_impl(run_ctx, run_count);
        } catch (const PluginCancelled &) {
            return;
        } catch (const std::exception &e) {
            report_error(run_ctx, e.what());
            return;
        } catch (...) {
            report_error(run_ctx, "Unknown C++ exception in plugin setup");
            return;
        }
    }

    void setup_run_safe(const plugin_run_context *run_ctx) const noexcept
    {
        try {
            setup_run_impl(run_ctx);
        } catch (const PluginCancelled &) {
            return;
        } catch (const std::exception &e) {
            report_error(run_ctx, e.what());
            return;
        } catch (...) {
            report_error(run_ctx, "Unknown C++ exception in plugin setup_run");
            return;
        }
    }

    void run_safe(const plugin_run_context *run_ctx) const noexcept
    {
        try {
            run_impl(run_ctx);
        } catch (const PluginCancelled &) {
            return;
        } catch (const std::exception &e) {
            report_error(run_ctx, e.what());
            return;
        } catch (...) {
            report_error(run_ctx, "Unknown C++ exception in plugin");
            return;
        }
    }

    static const char *get_id_bridge(void *plugin_ctx)
    {
        return static_cast<PluginBase *>(plugin_ctx)->id_impl();
    }

    static const char *get_name_bridge(void *plugin_ctx)
    {
        return static_cast<PluginBase *>(plugin_ctx)->name_impl();
    }

    static const char *get_description_bridge(void *plugin_ctx)
    {
        return static_cast<PluginBase *>(plugin_ctx)->description_impl();
    }

    static const char *get_exclusive_group_bridge(void *plugin_ctx)
    {
        return static_cast<PluginBase *>(plugin_ctx)->exclusive_group_impl();
    }

    static const char *get_exclusive_group_label_bridge(void *plugin_ctx)
    {
        return static_cast<PluginBase *>(plugin_ctx)->exclusive_group_label_impl();
    }

    static const char *get_exclusive_group_tooltip_bridge(void *plugin_ctx)
    {
        return static_cast<PluginBase *>(plugin_ctx)->exclusive_group_tooltip_impl();
    }

    static slicing_step_t get_step_bridge(void *plugin_ctx)
    {
        return static_cast<PluginBase *>(plugin_ctx)->step_impl();
    }

    static const_strings_t get_dependencies_bridge(void *plugin_ctx)
    {
        const char *const *deps = static_cast<PluginBase *>(plugin_ctx)->dependencies_impl();
        uint32_t count = 0;
        while (deps != nullptr && deps[count] != nullptr)
            ++count;

        const_strings_t out = {};
        out.items = deps;
        out.size = count;
        return out;
    }

    static int32_t get_priority_bridge(void *plugin_ctx)
    {
        return static_cast<PluginBase *>(plugin_ctx)->priority_impl();
    }

    static int32_t used_config_keys_bridge(void *plugin_ctx, raw_used_config_key *keys)
    {
        return static_cast<PluginBase *>(plugin_ctx)->used_config_keys(keys);
    }

    static int32_t defined_config_keys_bridge(void *plugin_ctx, const char **keys)
    {
        return static_cast<PluginBase *>(plugin_ctx)->defined_config_keys(keys);
    }

    static void initialize_bridge(void *plugin_ctx, storage_handle *storage)
    {
        static_cast<PluginBase *>(plugin_ctx)->initialize_safe(storage);
    }

    static void setup_bridge(void *plugin_ctx, const plugin_run_context *run_ctx, uint32_t run_count)
    {
        static_cast<PluginBase *>(plugin_ctx)->setup_safe(run_ctx, run_count);
    }

    static void setup_run_bridge(void *plugin_ctx, const plugin_run_context *run_ctx)
    {
        static_cast<PluginBase *>(plugin_ctx)->setup_run_safe(run_ctx);
    }

    static void run_bridge(void *plugin_ctx, const plugin_run_context *run_ctx)
    {
        static_cast<PluginBase *>(plugin_ctx)->run_safe(run_ctx);
    }

    static const plugin_vtable &vtable()
    {
        static const plugin_vtable vt = {
            SLIC3R_PLUGIN_ABI_VERSION,
            &PluginBase::get_id_bridge,
            &PluginBase::get_name_bridge,
            &PluginBase::get_description_bridge,
            &PluginBase::get_exclusive_group_bridge,
            &PluginBase::get_exclusive_group_label_bridge,
            &PluginBase::get_exclusive_group_tooltip_bridge,
            &PluginBase::get_step_bridge,
            &PluginBase::get_dependencies_bridge,
            &PluginBase::get_priority_bridge,
            &PluginBase::used_config_keys_bridge,
            &PluginBase::defined_config_keys_bridge,
            &PluginBase::initialize_bridge,
            &PluginBase::setup_bridge,
            &PluginBase::setup_run_bridge,
            &PluginBase::run_bridge
        };
        return vt;
    }

    mutable PluginProgress m_progress;

protected:
    orchestrator_handle *m_orchestrator;
};

} // namespace slic3r_api


#endif // slic3r_Api_plugin_cpp_PluginBase_hpp_
