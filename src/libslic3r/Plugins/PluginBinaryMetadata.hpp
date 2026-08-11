///|/ Copyright (c) SuperSlicer 2026 Durand Remi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/

// PluginBinaryMetadata is a small, platform-neutral record embedded directly
// into native plugin binaries. The repository cache scans the file for its
// magic bytes, so it can recover package and slicer versions without loading
// or executing plugin code. Keep this structure pointer-free and fixed-size:
// generated plugins and older slicers must agree on its exact byte layout.

#ifndef slic3r_Plugins_PluginBinaryMetadata_hpp_
#define slic3r_Plugins_PluginBinaryMetadata_hpp_

#include <array>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace Slic3r {

constexpr uint32_t PLUGIN_BINARY_METADATA_FORMAT_VERSION = 1;
constexpr size_t PLUGIN_BINARY_METADATA_TEXT_CAPACITY = 64;
constexpr std::array<char, 16> PLUGIN_BINARY_METADATA_MAGIC = {
    'S', 'L', 'I', 'C', '3', 'R', '_', 'P', 'L', 'U', 'G', 'I', 'N', '_', 'V', '1'
};

struct PluginBinaryMetadata
{
    std::array<char, 16> magic;
    uint32_t format_version;
    char package_version[PLUGIN_BINARY_METADATA_TEXT_CAPACITY];
    char slicer_version[PLUGIN_BINARY_METADATA_TEXT_CAPACITY];
};

static_assert(std::is_standard_layout<PluginBinaryMetadata>::value,
              "Plugin binary metadata must have a stable C-compatible layout");
static_assert(sizeof(PluginBinaryMetadata) == 148,
              "Changing plugin binary metadata requires a new format version");

} // namespace Slic3r

#endif // slic3r_Plugins_PluginBinaryMetadata_hpp_
