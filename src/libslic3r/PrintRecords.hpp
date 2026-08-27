///|/ Copyright (c) SuperSlicer 2026 Durand Remi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/
#ifndef slic3r_PrintRecords_hpp_
#define slic3r_PrintRecords_hpp_

#include <atomic>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "Config/ConfigDef.hpp"

namespace Slic3r {

using PrintRecordId = uint32_t;

constexpr PrintRecordId INVALID_PRINT_RECORD_ID = 0;

/*
Owns variable, plugin-readable data associated with one Print.

Each channel contains one DynamicConfig. The store deliberately assigns no
schema to that configuration: a producer may use scalar options as a property
bag or parallel vector options as a column-oriented table. The empty channel is
valid and provides a shared location for generic print metadata.

Channel mutation is intentionally not synchronized. Producers must write from
a sequential part of the pipeline, then leave the channel unchanged while
parallel readers use it. Only allocate_id() is thread-safe, so workers may
reserve stable references while preparing data outside this store.
*/
class PrintRecordStore
{
public:
    const DynamicConfig *find(const std::string &channel) const;
    DynamicConfig *find_mutable(const std::string &channel);
    DynamicConfig &get_or_add(const std::string &channel);
    bool remove(const std::string &channel);
    std::vector<std::string> channels() const;

    /* Remove every channel and restart the per-Print identifier sequence. */
    void clear();

    /*
    Reserve an identifier that fits in ConfigOptionInt / ConfigOptionInts.

    Zero denotes failure. Identifiers are never reused before clear(), even if
    the channel that stored one of them is removed.
    */
    PrintRecordId allocate_id();

private:
    std::unordered_map<std::string, DynamicConfig> m_channels;
    std::atomic<PrintRecordId> m_next_id { 1 };
};

} // namespace Slic3r

#endif // slic3r_PrintRecords_hpp_
