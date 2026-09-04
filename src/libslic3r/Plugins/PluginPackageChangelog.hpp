// Optional, release-authored notes shared by local discovery and the updater.
// A present empty result is intentional; only an absent version permits the
// remote changelog fallback. Invalid notes never affect ABI compatibility.
#ifndef slic3r_PluginPackageChangelog_hpp_
#define slic3r_PluginPackageChangelog_hpp_

#include <optional>
#include <string>

namespace Slic3r {
// Validate the complete document, then select the exact package version.
// Notes are plain text bullets, not executable HTML or rendered Markdown.
bool parse_plugin_package_changelog(const std::string &text, const std::string &version,
                                    std::optional<std::string> &notes, std::string &error);
// Read at most 1 MiB. A missing optional file succeeds with no notes; other
// failures include the file path in error and leave notes absent.
bool read_plugin_package_changelog(const std::string &path, const std::string &version,
                                   std::optional<std::string> &notes, std::string &error);
}
#endif
