///|/ Copyright (c) SuperSlicer 2026 Durand Rémi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/
#include "PropertyStorage.hpp"

#include <algorithm>
#include <cstring>
#include <new>

namespace Slic3r {

size_t PropertyRawBuffer::normalized_alignment(size_t alignment)
{
    if (alignment == 0)
        alignment = 1;

    /*
    Plugin ABI callers pass alignof(Payload). Align at least to max_align_t so
    ordinary C structs stay safe even if an older plugin accidentally provides
    a smaller value.
    */
    return std::max(alignment, alignof(std::max_align_t));
}

void *PropertyRawBuffer::allocate(size_t byte_count, size_t alignment)
{
    if (byte_count == 0)
        return nullptr;
    return ::operator new(byte_count, std::align_val_t(normalized_alignment(alignment)));
}

void PropertyRawBuffer::deallocate(void *data, size_t alignment)
{
    if (data != nullptr)
        ::operator delete(data, std::align_val_t(normalized_alignment(alignment)));
}

PropertyRawBuffer::PropertyRawBuffer(size_t byte_count, size_t alignment)
{
    assign_zeroed(byte_count, alignment);
}

PropertyRawBuffer::PropertyRawBuffer(const void *data, size_t byte_count, size_t alignment)
{
    assign_copy(data, byte_count, alignment);
}

PropertyRawBuffer::PropertyRawBuffer(const PropertyRawBuffer &rhs)
{
    assign_copy(rhs.m_data, rhs.m_byte_count, rhs.m_alignment);
}

PropertyRawBuffer::PropertyRawBuffer(PropertyRawBuffer &&rhs) noexcept :
    m_data(rhs.m_data),
    m_byte_count(rhs.m_byte_count),
    m_alignment(rhs.m_alignment)
{
    rhs.m_data = nullptr;
    rhs.m_byte_count = 0;
    rhs.m_alignment = 0;
}

PropertyRawBuffer::~PropertyRawBuffer()
{
    reset();
}

PropertyRawBuffer &PropertyRawBuffer::operator=(const PropertyRawBuffer &rhs)
{
    if (this != &rhs)
        assign_copy(rhs.m_data, rhs.m_byte_count, rhs.m_alignment);
    return *this;
}

PropertyRawBuffer &PropertyRawBuffer::operator=(PropertyRawBuffer &&rhs) noexcept
{
    if (this == &rhs)
        return *this;

    reset();
    m_data = rhs.m_data;
    m_byte_count = rhs.m_byte_count;
    m_alignment = rhs.m_alignment;
    rhs.m_data = nullptr;
    rhs.m_byte_count = 0;
    rhs.m_alignment = 0;
    return *this;
}

void PropertyRawBuffer::reset()
{
    deallocate(m_data, m_alignment);
    m_data = nullptr;
    m_byte_count = 0;
    m_alignment = 0;
}

void PropertyRawBuffer::assign_zeroed(size_t byte_count, size_t alignment)
{
    reset();
    if (byte_count == 0)
        return;

    m_alignment = normalized_alignment(alignment);
    m_byte_count = byte_count;
    m_data = allocate(byte_count, m_alignment);
    std::memset(m_data, 0, byte_count);
}

void PropertyRawBuffer::assign_copy(const void *data, size_t byte_count, size_t alignment)
{
    assign_zeroed(byte_count, alignment);
    if (m_data != nullptr && data != nullptr)
        std::memcpy(m_data, data, byte_count);
}

PropertyStorageSlot::PropertyStorageSlot(slic3r_property_type type, size_t byte_count, size_t alignment) :
    m_buffer(byte_count, alignment),
    m_type(type)
{
}

PropertyStorageSlot::PropertyStorageSlot(slic3r_property_type type,
                                         const void *data,
                                         size_t byte_count,
                                         size_t alignment) :
    m_buffer(data, byte_count, alignment),
    m_type(type)
{
}

bool PropertyStorageSlot::same_payload(const PropertyStorageSlot &rhs) const
{
    if (m_type != rhs.m_type || byte_count() != rhs.byte_count() || alignment() != rhs.alignment())
        return false;
    if (byte_count() == 0)
        return true;
    return std::memcmp(data(), rhs.data(), byte_count()) == 0;
}

void PropertyStorageSlot::reset()
{
    m_buffer.reset();
    m_type = SLIC3R_PROPERTY_TYPE_INVALID;
}

void PropertyStorageSlot::emplace_raw(slic3r_property_type type,
                                      const void *data,
                                      size_t byte_count,
                                      size_t alignment)
{
    m_buffer.assign_copy(data, byte_count, alignment);
    m_type = type;
}

void PropertyStorageSlot::emplace_zeroed(slic3r_property_type type,
                                         size_t byte_count,
                                         size_t alignment)
{
    m_buffer.assign_zeroed(byte_count, alignment);
    m_type = type;
}

} // namespace Slic3r
