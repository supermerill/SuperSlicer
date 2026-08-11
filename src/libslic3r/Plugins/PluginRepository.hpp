///|/ Copyright (c) SuperSlicer 2026 Durand Remi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/

#ifndef plugins_pluginrepository_hpp_
#define plugins_pluginrepository_hpp_

#include <map>
#include <set>
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

// Parse either a repository descriptor or a version description. Root
// descriptors intentionally omit versions; package adapters decide whether
// missing version fields may receive local-content defaults.
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

// Return the root containing one repository descriptor, tags, logs and all
// cached versions. The physical name is sanitized; package INI files retain
// the original repository id.
boost::filesystem::path repository_cache_root_path(const boost::filesystem::path &data_directory,
                                                   RepositoryPackageType type,
                                                   const std::string &package_name);

// Return one exact cached version. Vendors and plugins use the same
// <content-version>=<slicer-version> directory convention.
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

// The activation file separates package lifecycle from plugin activation. A
// package version selected in [installed] is copied into the live directory on
// the next launch. A package listed in [removed] is deleted on that launch.
// Individual plugin ids remain independently enabled in [activated].
struct PluginActivationConfig {
    std::map<std::string, bool> activated;
    std::map<std::string, PluginInstalledVersion> installed;
    std::set<std::string> removed;
};

// Return the user configuration file which records package changes and enabled
// plugin ids.
boost::filesystem::path plugin_activation_config_path(const boost::filesystem::path &data_directory);

// Read or write the [installed], [removed] and [activated] sections. Callers
// load the complete value before changing one concern, so unrelated package
// requests and activation choices remain present when the file is rewritten.
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

// Prepare the current cache layout and extract shipped ZIP bundles. After a
// layout purge, live packages are recached and requests whose only copy was in
// the obsolete cache are removed before the loader applies package changes.
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

// Validate a cached package without changing either the cache or the live
// plugin directory. Updaters use this before deciding whether a network
// download is necessary.
bool plugin_package_cache_is_valid(const boost::filesystem::path &data_directory,
                                   const std::string &package_name,
                                   const PluginInstalledVersion &version,
                                   std::string &error_message);

// Apply the package requests read from activated.ini before loading any DLL.
// Missing removal targets are non-fatal and returned through warnings. Fatal
// validation or filesystem failures return false through error_message.
bool apply_requested_plugin_package_changes(const boost::filesystem::path &data_directory,
                                            PluginActivationConfig &config,
                                            std::vector<std::string> &warnings,
                                            std::string &error_message);

// Schedule a cached package version for installation on the next process
// start. The current process never replaces a loaded plugin library.
bool request_plugin_install(const std::string &package_name,
                            const std::string &package_version,
                            const std::string &slicer_version,
                            std::string &error_message);

// Schedule removal for the next process start. The running process keeps its
// loaded DLL and files untouched until the loader applies this request.
bool request_plugin_uninstall(const std::string &package_name, std::string &error_message);

} // namespace Slic3r

#endif // plugins_pluginrepository_hpp_
