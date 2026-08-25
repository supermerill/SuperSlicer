///|/ Copyright (c) SuperSlicer 2026 Durand Remi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/
#ifndef slic3r_Api_plugin_cpp_gcode_GCodeFirmwareViews_hpp_
#define slic3r_Api_plugin_cpp_gcode_GCodeFirmwareViews_hpp_

#include <cstddef>
#include <exception>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "libslic3r/Api/plugin/c/slic3r_gcode_firmware.h"
#include "libslic3r/Api/plugin/cpp/DataTreeViews.hpp"
#include "libslic3r/Api/plugin/cpp/PrintingPlanViews.hpp"
#include "libslic3r/Point.hpp"

/*
G-code firmware C++ adapter
===========================

Firmware providers derive GCodeFirmwareSession and return one instance with
make_gcode_firmware_instance(). Their methods use normal C++ views and return
std::string values. The generated C thunks keep those strings in the session
holder, expose them as borrowed ABI text, and contain every exception.

GCodeFirmwareView is the consumer side used by STEP_GCODE writers. It validates
the complete instance once, then exposes each borrowed result as a string_view.
The view is valid only until the next method call on the same firmware session.
*/

namespace slic3r_api {

class GCodeFirmwareSession
{
public:
    /*
    One instance represents one complete export and may keep machine state in
    normal member variables. The writer calls the methods below in nested
    order:

        begin_print
          begin_group
            begin_layer
              begin_tool_group
                write_event(before)
                write_extrusion ...
                write_event(after)
              end_tool_group
            end_layer
          end_group
        end_print

    Each returned string is appended verbatim to the output file, so the
    firmware is responsible for separators and line endings. Returning an
    empty string is valid. The adapter catches exceptions and reports them
    through the C ABI instead of allowing them to cross the plugin boundary.

    The Print and PrintingPlan views passed to these methods are borrowed. A
    session may inspect them during the call but must not retain the views or
    their handles after the method returns.
    */
    virtual ~GCodeFirmwareSession() = default;

    // Called exactly once before any plan group. Emit the file preamble and
    // initialize state that depends on the complete Print configuration.
    virtual std::string begin_print(const Print &) = 0;

    // Enter one PrintingGroup. Groups represent independent ordered batches;
    // the preceding group has already received end_group().
    virtual std::string begin_group(const PrintingGroup &) = 0;

    // Enter one layer of the current group before any of its tool sections.
    // Layer-related transitions such as a future Z change belong here.
    virtual std::string begin_layer(const PrintingLayerGroup &) = 0;

    // Enter an ordered section for one tool. Future tool selection and
    // restoration of tool-specific state belong at this boundary.
    virtual std::string begin_tool_group(const PrintingToolGroup &) = 0;

    // Serialize one PrintingExtrusion in its stored order. The firmware owns
    // the policy for traversing its extrusion root and interpreting its
    // inherited properties; the file writer does not inspect that geometry.
    virtual std::string write_extrusion(const PrintingExtrusion &) = 0;

    // Serialize one ordered scope-event tree. The matching begin_* callback
    // has already established the scope context, and the matching end_* has
    // not run yet. The event has no source-region metadata.
    virtual std::string write_event(const ExtrusionEntity &) = 0;

    // Leave the current tool section after all its extrusions were written.
    // The session must retain any context needed here because no view is passed.
    virtual std::string end_tool_group() = 0;

    // Leave the current layer after every tool section has ended.
    virtual std::string end_layer() = 0;

    // Leave the current PrintingGroup after every layer has ended.
    virtual std::string end_group() = 0;

    // Called exactly once after all groups. Emit final commands and close any
    // firmware-level structure before the output file is published.
    virtual std::string end_print() = 0;

};

namespace detail {

struct GCodeFirmwareSessionHolder
{
    std::unique_ptr<GCodeFirmwareSession> session;
    std::string output;
    std::string error;
};

inline void initialize_firmware_result(raw_gcode_firmware_result *result) noexcept
{
    result->status = RAW_GCODE_FIRMWARE_STATUS_UNSET;
    result->text = nullptr;
    result->text_size = 0;
    result->error_message = nullptr;
}

template<class Callback>
inline void invoke_firmware_callback(void *opaque,
                                     raw_gcode_firmware_result *result,
                                     Callback callback) noexcept
{
    // A truncated result does not provide enough writable storage for the
    // status and pointers, so reject it without touching memory beyond the
    // caller-declared structure.
    if (result == nullptr || result->struct_size < sizeof(raw_gcode_firmware_result))
        return;

    initialize_firmware_result(result);
    if (opaque == nullptr) {
        result->status = RAW_GCODE_FIRMWARE_STATUS_INVALID_ARGUMENT;
        return;
    }

    GCodeFirmwareSessionHolder *holder = static_cast<GCodeFirmwareSessionHolder *>(opaque);
    try {
        holder->error.clear();
        holder->output = callback(*holder->session);
        result->status = RAW_GCODE_FIRMWARE_STATUS_SUCCESS;
        result->text = holder->output.data();
        result->text_size = static_cast<uint64_t>(holder->output.size());
    } catch (const std::exception &exception) {
        holder->output.clear();
        holder->error = exception.what();
        result->status = RAW_GCODE_FIRMWARE_STATUS_ERROR;
        result->error_message = holder->error.c_str();
    } catch (...) {
        holder->output.clear();
        holder->error = "Unknown exception in the G-code firmware session.";
        result->status = RAW_GCODE_FIRMWARE_STATUS_ERROR;
        result->error_message = holder->error.c_str();
    }
}

inline void destroy_firmware_session(void *opaque) noexcept
{
    delete static_cast<GCodeFirmwareSessionHolder *>(opaque);
}

inline void begin_print_thunk(void *opaque,
                              const print_handle *print,
                              raw_gcode_firmware_result *result) noexcept
{
    invoke_firmware_callback(opaque, result, [print](GCodeFirmwareSession &session) {
        if (print == nullptr)
            throw std::invalid_argument("Firmware begin_print received a null Print.");
        return session.begin_print(Print(print));
    });
}

inline void begin_group_thunk(void *opaque,
                              const printing_group_handle *group,
                              raw_gcode_firmware_result *result) noexcept
{
    invoke_firmware_callback(opaque, result, [group](GCodeFirmwareSession &session) {
        if (group == nullptr)
            throw std::invalid_argument("Firmware begin_group received a null group.");
        return session.begin_group(PrintingGroup(group));
    });
}

inline void begin_layer_thunk(void *opaque,
                              const printing_layer_group_handle *layer_group,
                              raw_gcode_firmware_result *result) noexcept
{
    invoke_firmware_callback(opaque, result, [layer_group](GCodeFirmwareSession &session) {
        if (layer_group == nullptr)
            throw std::invalid_argument("Firmware begin_layer received a null layer group.");
        return session.begin_layer(PrintingLayerGroup(layer_group));
    });
}

inline void begin_tool_group_thunk(void *opaque,
                                   const printing_tool_group_handle *tool_group,
                                   raw_gcode_firmware_result *result) noexcept
{
    invoke_firmware_callback(opaque, result, [tool_group](GCodeFirmwareSession &session) {
        if (tool_group == nullptr)
            throw std::invalid_argument("Firmware begin_tool_group received a null tool group.");
        return session.begin_tool_group(PrintingToolGroup(tool_group));
    });
}

inline void write_extrusion_thunk(void *opaque,
                                  const printing_extrusion_handle *extrusion,
                                  raw_gcode_firmware_result *result) noexcept
{
    invoke_firmware_callback(opaque, result, [extrusion](GCodeFirmwareSession &session) {
        if (extrusion == nullptr)
            throw std::invalid_argument("Firmware write_extrusion received a null extrusion.");
        return session.write_extrusion(PrintingExtrusion(extrusion));
    });
}

inline void write_event_thunk(void *opaque,
                              const extrusion_entity_handle *event_root,
                              raw_gcode_firmware_result *result) noexcept
{
    invoke_firmware_callback(opaque, result, [event_root](GCodeFirmwareSession &session) {
        if (event_root == nullptr)
            throw std::invalid_argument("Firmware write_event received a null event root.");
        return session.write_event(ExtrusionEntity(event_root));
    });
}

inline void end_tool_group_thunk(void *opaque, raw_gcode_firmware_result *result) noexcept
{
    invoke_firmware_callback(opaque, result, [](GCodeFirmwareSession &session) {
        return session.end_tool_group();
    });
}

inline void end_layer_thunk(void *opaque, raw_gcode_firmware_result *result) noexcept
{
    invoke_firmware_callback(opaque, result, [](GCodeFirmwareSession &session) {
        return session.end_layer();
    });
}

inline void end_group_thunk(void *opaque, raw_gcode_firmware_result *result) noexcept
{
    invoke_firmware_callback(opaque, result, [](GCodeFirmwareSession &session) {
        return session.end_group();
    });
}

inline void end_print_thunk(void *opaque, raw_gcode_firmware_result *result) noexcept
{
    invoke_firmware_callback(opaque, result, [](GCodeFirmwareSession &session) {
        return session.end_print();
    });
}

inline const raw_gcode_firmware_vtable &firmware_session_vtable()
{
    static const raw_gcode_firmware_vtable vtable = {
        sizeof(raw_gcode_firmware_vtable),
        &destroy_firmware_session,
        &begin_print_thunk,
        &begin_group_thunk,
        &begin_layer_thunk,
        &begin_tool_group_thunk,
        &write_extrusion_thunk,
        &write_event_thunk,
        &end_tool_group_thunk,
        &end_layer_thunk,
        &end_group_thunk,
        &end_print_thunk
    };
    return vtable;
}

} // namespace detail

inline raw_gcode_firmware_instance make_gcode_firmware_instance(
    std::unique_ptr<GCodeFirmwareSession> session)
{
    if (!session)
        throw std::invalid_argument("A firmware instance needs a session object.");

    detail::GCodeFirmwareSessionHolder *holder = new detail::GCodeFirmwareSessionHolder();
    holder->session = std::move(session);

    raw_gcode_firmware_instance instance = {};
    instance.struct_size = sizeof(instance);
    instance.session = holder;
    instance.vtable = &detail::firmware_session_vtable();
    return instance;
}

class GCodeFirmwareView
{
public:
    explicit GCodeFirmwareView(const raw_gcode_firmware_instance *instance) : m_instance(instance)
    {
        validate();
    }

    std::string_view begin_print(const Print &print) const
    {
        raw_gcode_firmware_result result = make_result();
        m_instance->vtable->begin_print(m_instance->session, print.handle(), &result);
        return consume(result);
    }

    std::string_view begin_group(const PrintingGroup &group) const
    {
        raw_gcode_firmware_result result = make_result();
        m_instance->vtable->begin_group(m_instance->session, group.handle(), &result);
        return consume(result);
    }

    std::string_view begin_layer(const PrintingLayerGroup &layer_group) const
    {
        raw_gcode_firmware_result result = make_result();
        m_instance->vtable->begin_layer(m_instance->session, layer_group.handle(), &result);
        return consume(result);
    }

    std::string_view begin_tool_group(const PrintingToolGroup &tool_group) const
    {
        raw_gcode_firmware_result result = make_result();
        m_instance->vtable->begin_tool_group(m_instance->session, tool_group.handle(), &result);
        return consume(result);
    }

    std::string_view write_extrusion(const PrintingExtrusion &extrusion) const
    {
        raw_gcode_firmware_result result = make_result();
        m_instance->vtable->write_extrusion(m_instance->session, extrusion.handle(), &result);
        return consume(result);
    }

    std::string_view write_event(const ExtrusionEntity &event_root) const
    {
        raw_gcode_firmware_result result = make_result();
        m_instance->vtable->write_event(m_instance->session, event_root.handle(), &result);
        return consume(result);
    }

    std::string_view end_tool_group() const { return invoke_end(m_instance->vtable->end_tool_group); }
    std::string_view end_layer() const { return invoke_end(m_instance->vtable->end_layer); }
    std::string_view end_group() const { return invoke_end(m_instance->vtable->end_group); }
    std::string_view end_print() const { return invoke_end(m_instance->vtable->end_print); }

private:
    raw_gcode_firmware_result make_result() const
    {
        raw_gcode_firmware_result result = {};
        result.struct_size = sizeof(result);
        result.status = RAW_GCODE_FIRMWARE_STATUS_UNSET;
        return result;
    }

    std::string_view invoke_end(gcode_firmware_end_scope_fn callback) const
    {
        raw_gcode_firmware_result result = make_result();
        callback(m_instance->session, &result);
        return consume(result);
    }

    std::string_view consume(const raw_gcode_firmware_result &result) const
    {
        if (result.status != RAW_GCODE_FIRMWARE_STATUS_SUCCESS) {
            const char *message = result.error_message != nullptr ?
                result.error_message : "The G-code firmware callback failed.";
            throw std::runtime_error(message);
        }
        if (result.text == nullptr && result.text_size != 0)
            throw std::runtime_error("The G-code firmware returned invalid text.");
        return result.text != nullptr ?
            std::string_view(result.text, static_cast<size_t>(result.text_size)) : std::string_view();
    }

    void validate() const
    {
        if (m_instance == nullptr || m_instance->struct_size < sizeof(raw_gcode_firmware_instance) ||
            m_instance->session == nullptr || m_instance->vtable == nullptr)
            throw std::invalid_argument("The G-code firmware instance is incomplete.");

        const raw_gcode_firmware_vtable &vtable = *m_instance->vtable;
        if (vtable.struct_size < sizeof(raw_gcode_firmware_vtable) || vtable.destroy == nullptr ||
            vtable.begin_print == nullptr || vtable.begin_group == nullptr ||
            vtable.begin_layer == nullptr || vtable.begin_tool_group == nullptr ||
            vtable.write_extrusion == nullptr || vtable.write_event == nullptr ||
            vtable.end_tool_group == nullptr ||
            vtable.end_layer == nullptr || vtable.end_group == nullptr || vtable.end_print == nullptr)
            throw std::invalid_argument("The G-code firmware callback table is incomplete.");
    }

    const raw_gcode_firmware_instance *m_instance = nullptr;
};

} // namespace slic3r_api

#endif // slic3r_Api_plugin_cpp_gcode_GCodeFirmwareViews_hpp_
