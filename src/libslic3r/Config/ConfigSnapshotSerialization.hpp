///|/ Copyright (c) SuperSlicer 2026 Durand Remi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/

#ifndef slic3r_ConfigSnapshotSerialization_hpp_
#define slic3r_ConfigSnapshotSerialization_hpp_

/*
Versioned serialization for self-contained configuration snapshots.

A snapshot stores option keys, values, types and flags for transport or later
merge. It does not serialize option definitions, preset metadata or the
application's usual profile files. The encoder accepts any ConfigBase, while
the decoder builds an independent DynamicConfig so callers may safely merge,
replace or discard the decoded values after complete validation.
*/

#include <memory>
#include <string>

#include "libslic3r/Api/plugin/c/slic3r_config_option_type.h"

namespace Slic3r {

class ConfigBase;
class DynamicConfig;
class ConfigOption;

namespace ConfigSnapshotSerialization {

// Serialize every option into the deterministic SCFG representation.
bool serialize_all(const ConfigBase &source, std::string &output);

// Replace output only after the complete SCFG document has been validated.
bool deserialize_all(const std::string &input, DynamicConfig &output);

namespace Detail {

// The host's get-or-add adapter must create exactly the same option types that
// the transport can later serialize. These helpers are shared implementation
// details rather than part of the configuration serialization interface.
std::unique_ptr<ConfigOption> create_empty_option(config_option_type type);
bool supports_option_type(config_option_type type);

} // namespace Detail

} // namespace ConfigSnapshotSerialization
} // namespace Slic3r

#endif // slic3r_ConfigSnapshotSerialization_hpp_
