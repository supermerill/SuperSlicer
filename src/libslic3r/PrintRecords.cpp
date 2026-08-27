///|/ Copyright (c) SuperSlicer 2026 Durand Remi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/

/*
Implementation of the Print-owned record channels.

The unordered map provides name-based lookup without imposing an iteration
order. channels() sorts a snapshot before exposing it so diagnostics and plugin
behavior never depend on the container's bucket layout. Identifier allocation
is independent from channel storage and uses relaxed atomics because only
uniqueness, not cross-thread ordering, is required.
*/

#include "PrintRecords.hpp"

#include <algorithm>
#include <limits>
#include <utility>

namespace Slic3r {

const DynamicConfig *PrintRecordStore::find(const std::string &channel) const
{
    std::unordered_map<std::string, DynamicConfig>::const_iterator it = m_channels.find(channel);
    return it == m_channels.end() ? nullptr : &it->second;
}

DynamicConfig *PrintRecordStore::find_mutable(const std::string &channel)
{
    std::unordered_map<std::string, DynamicConfig>::iterator it = m_channels.find(channel);
    return it == m_channels.end() ? nullptr : &it->second;
}

DynamicConfig &PrintRecordStore::get_or_add(const std::string &channel)
{
    return m_channels.try_emplace(channel).first->second;
}

bool PrintRecordStore::remove(const std::string &channel)
{
    return m_channels.erase(channel) != 0;
}

std::vector<std::string> PrintRecordStore::channels() const
{
    std::vector<std::string> result;
    result.reserve(m_channels.size());
    for (const std::pair<const std::string, DynamicConfig> &entry : m_channels)
        result.emplace_back(entry.first);
    std::sort(result.begin(), result.end());
    return result;
}

void PrintRecordStore::clear()
{
    m_channels.clear();
    m_next_id.store(1, std::memory_order_relaxed);
}

PrintRecordId PrintRecordStore::allocate_id()
{
    constexpr PrintRecordId max_config_int = static_cast<PrintRecordId>(std::numeric_limits<int32_t>::max());
    PrintRecordId candidate = m_next_id.load(std::memory_order_relaxed);

    /*
    Stop rather than wrapping once every positive ConfigOptionInt value has
    been issued. compare_exchange also lets many workers reserve identifiers
    without serializing unrelated record construction.
    */
    while (candidate <= max_config_int) {
        const PrintRecordId next = candidate + 1;
        if (m_next_id.compare_exchange_weak(
                candidate, next, std::memory_order_relaxed, std::memory_order_relaxed))
            return candidate;
    }
    return INVALID_PRINT_RECORD_ID;
}

} // namespace Slic3r
