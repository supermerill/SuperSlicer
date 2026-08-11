///|/ Copyright (c) SuperSlicer 2026 Durand Remi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/

#ifndef plugins_pluginrepository_hpp_
#define plugins_pluginrepository_hpp_

#include <map>
#include <string>
#include <vector>

#include <boost/filesystem/path.hpp>

namespace Slic3r {

// A repository may ship either presets or plugins. The updater keeps this
// small common description separate from the content-specific installation
// code, which lets both package kinds use the same discovery and cache rules.
enum class RepositoryPackageType {
    Vendor,
    Plugin
};

struct RepositoryDescription {
    RepositoryPackageType type = RepositoryPackageType::Plugin;
    std::string id;
    std::string name;
    std::string full_name;
    std::string description;
    std::string config_update_rest;
    std::string slicer;
    std::string package_version;
    std::string slicer_version;
};

struct RepositoryPackageVersion {
    std::string package_version;
    std::string slicer_version;
    std::string url_zip;
    std::string commit_sha;
    std::string commit_url;
    std::string tag;
};

// Parse the common description.ini stored both beside a repository and in a
// downloaded package. A plugin package must provide both version fields.
bool parse_repository_description(const std::string &contents,
                                  RepositoryPackageType expected_type,
                                  RepositoryDescription &description,
                                  std::string &error_message);

// GitHub tags use "package_version=slicer_version" for both vendors and
// plugins. Invalid tags are ignored so one malformed release cannot block a
// complete repository refresh.
bool parse_repository_versions(const std::string &json,
                               std::vector<RepositoryPackageVersion> &versions,
                               std::string &error_message);

// Keep cache paths deterministic while preserving underscores in package ids.
boost::filesystem::path repository_package_cache_path(const boost::filesystem::path &data_directory,
                                                      RepositoryPackageType type,
                                                      const std::string &package_name,
                                                      const std::string &package_version,
                                                      const std::string &slicer_version);

// Extract ZIP contents literally below destination after rejecting absolute
// paths and path traversal. Callers publish the temporary destination only
// after their package-specific validation succeeds.
bool extract_repository_archive(const boost::filesystem::path &archive_path,
                                const boost::filesystem::path &destination,
                                std::string &error_message);

struct PluginInstalledVersion {
    std::string package_version;
    std::string slicer_version;
};

// The activation file has two independent concerns. A package version is
// selected in [installed], while individual plugin ids are enabled in
// [activated] after that package has been loaded on the next launch.
struct PluginActivationConfig {
    std::map<std::string, bool> activated;
    std::map<std::string, PluginInstalledVersion> installed;
};

// Return the user configuration file which records requested package versions
// and enabled plugin ids.
boost::filesystem::path plugin_activation_config_path(const boost::filesystem::path &data_directory);

// Read or write both sections of activated.ini. Writing deliberately preserves
// [installed] when a caller changes only the enabled plugin ids.
bool read_plugin_activation_config(const boost::filesystem::path &config_path,
                                   PluginActivationConfig &config,
                                   std::string &error_message);
bool write_plugin_activation_config(const boost::filesystem::path &config_path,
                                    const PluginActivationConfig &config,
                                    std::string &error_message);

// Create data_dir/plugins/activated.ini from resources when needed. The old
// data_dir/plugin location is copied once so existing profiles keep both
// their activation state and their requested package versions.
bool ensure_plugin_activation_config(const boost::filesystem::path &data_directory,
                                     PluginActivationConfig &config,
                                     bool &from_user_config,
                                     std::string &error_message);

// Extract shipped ZIP bundles into a versioned cache, then install the
// versions requested by config. These functions use explicit roots so tests
// can exercise the filesystem behaviour without global application state.
bool prepare_plugin_bundle_cache(const boost::filesystem::path &resources_directory,
                                 const boost::filesystem::path &data_directory,
                                 std::string &error_message);

// Validate and publish one downloaded plugin archive into the versioned cache.
// The archive may either contain the package directly or use the single root
// directory added by GitHub ZIP downloads.
bool cache_plugin_package_archive(const boost::filesystem::path &data_directory,
                                  const boost::filesystem::path &archive_path,
                                  const std::string &package_name,
                                  const std::string &package_version,
                                  const std::string &slicer_version,
                                  std::string &error_message);
bool install_requested_plugin_packages(const boost::filesystem::path &data_directory,
                                       const PluginActivationConfig &config,
                                       std::string &error_message);

// Schedule a cached package version for installation on the next process
// start. The current process never replaces a loaded plugin library.
bool request_plugin_install(const std::string &package_name,
                            const std::string &package_version,
                            const std::string &slicer_version,
                            std::string &error_message);

} // namespace Slic3r

#endif // plugins_pluginrepository_hpp_
