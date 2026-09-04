// Shared ABI registry and package manifest reader. Both native loading and the
// package manager use the same named contracts; slicer_version is descriptive.
// An unread remote package is NotChecked. Once read, missing or invalid ABI
// metadata is Incompatible, never an implicit promise of compatibility.
#ifndef slic3r_PluginApiCompatibility_hpp_
#define slic3r_PluginApiCompatibility_hpp_

#include <optional>
#include <string>
#include <vector>
#include "libslic3r/Api/plugin/c/slic3r_plugin_types.h"

namespace Slic3r {
struct PluginApiRequirement {
    std::string header;
    slic3r_major_minor_version version{};
};
enum class PluginApiCompatibilityStatus { NotChecked, Compatible, Incompatible };
struct PluginApiIssue {
    std::string header;
    slic3r_major_minor_version required{};
    std::optional<slic3r_major_minor_version> available;
    std::string detail;
};
struct PluginApiCompatibility {
    PluginApiCompatibilityStatus status = PluginApiCompatibilityStatus::NotChecked;
    std::vector<PluginApiIssue> issues;
    // Combine diagnostics for logs/tooltips without discarding structured data.
    std::string message() const;
    bool compatible() const { return status == PluginApiCompatibilityStatus::Compatible; }
};
struct PluginPackageMetadata {
    std::string package_version;
    std::string slicer_version;
    std::vector<PluginApiRequirement> abi;
    PluginApiCompatibility compatibility;
};

// Numeric order matches the public DLL table. Names remain stable in INI files.
const std::vector<PluginApiRequirement> &host_plugin_api_contracts();
// Require the vtable contract, ignore 0.0 entries, and compare named contracts.
PluginApiCompatibility validate_plugin_api_requirements(const std::vector<PluginApiRequirement> &requirements);
// Adapt the dense DLL export to the same validator without indexing past host data.
PluginApiCompatibility validate_plugin_api_table(const std::vector<slic3r_major_minor_version> &versions);
// Read the plugin identity and ABI together. Invalid ABI is retained in the
// result; false means the INI/identity itself could not be read. No code is loaded.
bool read_plugin_package_metadata(const std::string &path, PluginPackageMetadata &metadata, std::string &error);
// Parse in-memory INI text for tools/tests. Header names containing dots are literal.
bool parse_plugin_package_metadata(const std::string &text, PluginPackageMetadata &metadata, std::string &error);
}
#endif
