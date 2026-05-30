///|/ Copyright (c) SuperSlicer 2026 Durand Rémi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/
#ifndef slic3r_PropertyStorage_hpp_
#define slic3r_PropertyStorage_hpp_

#include <cassert>
#include <cstddef>
#include <new>
#include <type_traits>
#include <utility>

#include "Api/plugin/c/slic3r_def.h"

namespace Slic3r {

/*
Shared storage backend for small typed property payloads.

Several parts of the slicer need to attach plugin or host metadata to objects:
extrusions, surfaces, layers, islands, and later probably more data-tree nodes.
Those containers have different public semantics, but they all need the same
low-level machinery:

- own a byte buffer aligned for one C payload struct;
- copy that buffer when the owning object is copied;
- keep the numeric property id next to the bytes;
- compare payloads when two objects may be merged.

This file provides only that mechanical storage layer. It intentionally does
not know whether a property means "overhang data", "dense infill priority" or
anything else. The caller must validate the property id through the orchestrator
before creating a slot, then pass the registered byte_count and alignment here.
*/

/*
Owns one aligned raw byte payload.

Use this when the object lifetime is simple byte ownership: the payload is
trivially copyable, so copying the buffer with memcpy is the correct operation.
Do not use it for std::string, std::vector, pointers owning memory, or any type
that needs a destructor. Higher-level containers enforce that rule with
static_asserts on their typed helpers.
*/
class PropertyRawBuffer
{
public:
    PropertyRawBuffer() = default;
    PropertyRawBuffer(size_t byte_count, size_t alignment);
    PropertyRawBuffer(const void *data, size_t byte_count, size_t alignment);
    PropertyRawBuffer(const PropertyRawBuffer &rhs);
    PropertyRawBuffer(PropertyRawBuffer &&rhs) noexcept;
    ~PropertyRawBuffer();

    PropertyRawBuffer &operator=(const PropertyRawBuffer &rhs);
    PropertyRawBuffer &operator=(PropertyRawBuffer &&rhs) noexcept;

    /* Release the current payload and leave the buffer empty. */
    void reset();

    /*
    Allocate a new payload and fill it with zero bytes.

    This is the default behavior for newly created properties. It lets plugin
    code initialize only the fields it needs without first reading
    uninitialized memory.
    */
    void assign_zeroed(size_t byte_count, size_t alignment);

    /* Replace the payload with an owned byte-for-byte copy of data. */
    void assign_copy(const void *data, size_t byte_count, size_t alignment);

    const void *data() const { return m_data; }
    void *data_mutable() { return m_data; }
    size_t byte_count() const { return m_byte_count; }
    size_t alignment() const { return m_alignment; }
    bool empty() const { return m_byte_count == 0; }

    template<class T> T &as()
    {
        assert(m_data != nullptr);
        assert(m_byte_count == sizeof(T));
        assert(m_alignment >= alignof(T));
        return *reinterpret_cast<T *>(m_data);
    }

    template<class T> const T &as() const
    {
        assert(m_data != nullptr);
        assert(m_byte_count == sizeof(T));
        assert(m_alignment >= alignof(T));
        return *reinterpret_cast<const T *>(m_data);
    }

    /*
    Normalize an ABI-provided alignment before allocation.

    The public API expects alignof(Payload). The storage accepts defensive
    values and raises them to at least max_align_t so ordinary C payload structs
    remain safe even if older plugin code passed a smaller alignment.
    */
    static size_t normalized_alignment(size_t alignment);

private:
    static void *allocate(size_t byte_count, size_t alignment);
    static void deallocate(void *data, size_t alignment);

    void *m_data = nullptr;
    size_t m_byte_count = 0;
    size_t m_alignment = 0;
};

class PropertyStorageSlot
{
public:
    PropertyStorageSlot() = default;
    PropertyStorageSlot(slic3r_property_type type, size_t byte_count, size_t alignment);
    PropertyStorageSlot(slic3r_property_type type, const void *data, size_t byte_count, size_t alignment);

    slic3r_property_type type() const { return m_type; }
    bool empty() const { return m_type == SLIC3R_PROPERTY_TYPE_INVALID; }
    const void *data() const { return m_buffer.data(); }
    void *data_mutable() { return m_buffer.data_mutable(); }
    size_t byte_count() const { return m_buffer.byte_count(); }
    size_t alignment() const { return m_buffer.alignment(); }
    /*
    Compare both the key and the exact binary payload.

    Surface cleanup uses this to avoid merging two areas that have the same
    surface type but different plugin metadata.
    */
    bool same_payload(const PropertyStorageSlot &rhs) const;

    /*
    Construct a C++ wrapper payload directly in the raw storage.

    This helper is mainly for host-side built-ins such as extrusion properties.
    External plugin ABI code usually goes through emplace_zeroed(), because it
    knows the payload only as bytes registered on the orchestrator.
    */
    template<class PropertyType, class... Args> PropertyType &emplace(Args&&... args)
    {
        static_assert(std::is_trivially_copyable<PropertyType>::value,
                      "Stored property payloads are copied as bytes and must be trivially copyable.");
        reset();
        m_buffer.assign_zeroed(sizeof(PropertyType), alignof(PropertyType));
        PropertyType *property = new (m_buffer.data_mutable()) PropertyType(std::forward<Args>(args)...);
        m_type = PropertyType::property_type;
        return *property;
    }

    template<class PropertyType> PropertyType *get_if()
    {
        return type() == PropertyType::property_type ? &m_buffer.as<PropertyType>() : nullptr;
    }

    template<class PropertyType> const PropertyType *get_if() const
    {
        return type() == PropertyType::property_type ? &m_buffer.as<PropertyType>() : nullptr;
    }

    template<class PropertyType> PropertyType &as()
    {
        assert(type() == PropertyType::property_type);
        return m_buffer.as<PropertyType>();
    }

    template<class PropertyType> const PropertyType &as() const
    {
        assert(type() == PropertyType::property_type);
        return m_buffer.as<PropertyType>();
    }

    void reset();

    /* Store an owned copy of an already initialized payload. */
    void emplace_raw(slic3r_property_type type, const void *data, size_t byte_count, size_t alignment);

    /* Create an empty payload slot with all bytes initialized to zero. */
    void emplace_zeroed(slic3r_property_type type, size_t byte_count, size_t alignment);

private:
    PropertyRawBuffer m_buffer;
    slic3r_property_type m_type = SLIC3R_PROPERTY_TYPE_INVALID;
};

} // namespace Slic3r

#endif // slic3r_PropertyStorage_hpp_
