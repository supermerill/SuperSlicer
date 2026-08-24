///|/ Copyright (c) SuperSlicer 2026 Durand Remi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/
#ifndef slic3r_Api_plugin_cpp_gcode_EncodableState_hpp_
#define slic3r_Api_plugin_cpp_gcode_EncodableState_hpp_

#include <optional>
#include <stdexcept>

/*
Generic firmware encoding registers
===================================

These value types implement the bookkeeping shared by firmware-controlled
settings. A requested value describes the current target, while an encoded
value records the last target successfully included in generated firmware
output. AwaitableEncodableState also remembers the last target encoded with an
explicit wait instruction.

Domain objects remain responsible for units, validation and transformations.
For example, a heater stores the uncorrected requested temperature but records
the effective temperature after applying its configured offset. This keeps the
register generic without confusing configuration with generated output state.
*/

namespace slic3r_api { namespace GCodeGeneration {

// Tracks one requested value and the last value represented by generated
// firmware output. The explicit *_for() and *_as() methods let a domain object
// transform a raw request before comparing or recording its encoded value.
template <class T> class EncodableState
{
public:
    std::optional<T> requested() const { return m_requested; }
    std::optional<T> encoded() const { return m_encoded; }

    void request(std::optional<T> value) { m_requested = value; }

    bool needs_encoding() const
    {
        return m_requested && needs_encoding_for(*m_requested);
    }

    bool needs_encoding_for(const T &effective_value) const
    {
        return m_requested && (!m_encoded || effective_value != *m_encoded);
    }

    void mark_encoded()
    {
        if (!m_requested)
            throw std::logic_error("Cannot mark a firmware state as encoded without a requested value.");
        mark_encoded_as(*m_requested);
    }

    void mark_encoded_as(const T &effective_value)
    {
        if (!m_requested)
            throw std::logic_error("Cannot mark a firmware state as encoded without a requested value.");
        m_encoded = effective_value;
    }

    void clear_encoded() { m_encoded.reset(); }

    void reset_runtime_state()
    {
        m_requested.reset();
        m_encoded.reset();
    }

    void synchronize_runtime_from(const EncodableState &source)
    {
        m_requested = source.m_requested;
        m_encoded = source.m_encoded;
    }

private:
    // Target requested by the current logical scope. It may still require a
    // domain-specific correction before it can be encoded.
    std::optional<T> m_requested;
    // Effective value carried by the last successfully generated output.
    std::optional<T> m_encoded;
};

// Extends an encodable value with the last target represented by output that
// explicitly waits. This records generated instructions, not sensor feedback
// or proof that the physical machine has reached the target.
template <class T> class AwaitableEncodableState
{
public:
    std::optional<T> requested() const { return m_encoding.requested(); }
    std::optional<T> encoded() const { return m_encoding.encoded(); }
    std::optional<T> encoded_with_wait() const { return m_encoded_with_wait; }

    void request(std::optional<T> value) { m_encoding.request(value); }
    bool needs_encoding() const { return m_encoding.needs_encoding(); }
    bool needs_encoding_for(const T &effective_value) const
    {
        return m_encoding.needs_encoding_for(effective_value);
    }

    bool needs_wait_encoding() const
    {
        const std::optional<T> target = m_encoding.requested();
        return target && needs_wait_encoding_for(*target);
    }

    bool needs_wait_encoding_for(const T &effective_value) const
    {
        return m_encoding.requested() &&
               (m_encoding.needs_encoding_for(effective_value) ||
                !m_encoded_with_wait || effective_value != *m_encoded_with_wait);
    }

    void mark_encoded() { m_encoding.mark_encoded(); }
    void mark_encoded_as(const T &effective_value) { m_encoding.mark_encoded_as(effective_value); }

    void mark_encoded_with_wait()
    {
        const std::optional<T> target = m_encoding.requested();
        if (!target)
            throw std::logic_error("Cannot mark a firmware state as encoded with wait without a requested value.");
        mark_encoded_with_wait_as(*target);
    }

    void mark_encoded_with_wait_as(const T &effective_value)
    {
        if (!m_encoding.requested())
            throw std::logic_error("Cannot mark a firmware state as encoded with wait without a requested value.");

        // A wait instruction also carries the normal target, so both encoding
        // histories become current only after that instruction is generated.
        m_encoding.mark_encoded_as(effective_value);
        m_encoded_with_wait = effective_value;
    }

    void clear_encoded() { m_encoding.clear_encoded(); }
    void clear_encoded_with_wait() { m_encoded_with_wait.reset(); }

    void reset_runtime_state()
    {
        m_encoding.reset_runtime_state();
        m_encoded_with_wait.reset();
    }

    void synchronize_runtime_from(const AwaitableEncodableState &source)
    {
        m_encoding.synchronize_runtime_from(source.m_encoding);
        m_encoded_with_wait = source.m_encoded_with_wait;
    }

private:
    EncodableState<T> m_encoding;
    // Last effective target included in an explicit wait instruction.
    std::optional<T> m_encoded_with_wait;
};

}} // namespace slic3r_api::GCodeGeneration

#endif // slic3r_Api_plugin_cpp_gcode_EncodableState_hpp_
