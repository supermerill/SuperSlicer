///|/ Copyright (c) SuperSlicer 2026 Durand Remi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/

// PluginRepository defines the durable package protocol shared by vendor and
// plugin repositories. description.ini carries stable repository identity;
// package versions and cache/install operations remain separate from the
// activated.ini integrity rules defined by PluginActivationConfig.

#ifndef plugins_pluginrepository_hpp_
#define plugins_pluginrepository_hpp_

#include <string>
#include <vector>

#ifdef SLIC3R_PLUGIN_REPOSITORY_TESTING
#include <functional>
#endif

#include <boost/filesystem/path.hpp>

#include "PluginActivationConfig.hpp"
#include "PluginApiCompatibility.hpp"

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
    // Internal packages provide runtime support to other packages and may
    // legitimately register no plugin id of their own. They still use the
    // normal package lifecycle and remain visible in the package updater.
    bool is_internal = false;
};

struct RepositoryPackageVersion {
    std::string package_version;
    std::string slicer_version;
    std::string url_zip;
    std::string commit_sha;
    std::string commit_url;
    std::string tag;
};

// Parse repository-level identity and display metadata. Version-looking keys
// are deliberately ignored because versions belong to a vendor profile or a
// plugin version.ini, never to description.ini.
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

// Prepare the current cache layout without importing shipped bundles. Runtime
// repository operations use this entry point so clearing a bundled plugin
// remains effective until the application starts again.
bool prepare_plugin_cache(const boost::filesystem::path &data_directory,
                          std::string &error_message);

// Prepare the current cache layout and extract shipped ZIP bundles. The plugin
// loader calls this once during application startup. After a layout purge,
// desired live packages are recached before startup reconciliation validates
// the complete [installed] set.
bool prepare_plugin_bundle_cache(const boost::filesystem::path &resources_directory,
                                 const boost::filesystem::path &data_directory,
                                 std::string &error_message);

// Startup has already parsed and possibly sanitized activated.ini. Supplying
// that desired state keeps a cache-layout rebuild from reparsing the preserved
// partial file with the strict mutation reader.
bool prepare_plugin_bundle_cache(const boost::filesystem::path &resources_directory,
                                 const boost::filesystem::path &data_directory,
                                 const PluginActivationConfig &activation_config,
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

// Independent of cache structure: reject incompatible/undeclared contracts
// before scheduling, publishing or loading an installed package.
bool plugin_package_is_compatible(const boost::filesystem::path &package_root,
                                  std::string &error_message);

// Reconcile data/plugins with the complete desired package set from
// activated.ini before loading any DLL. False guarantees that the live tree
// was never changed or was fully restored. An incomplete rollback throws
// RuntimeError so startup cannot load a mixed package set.
bool reconcile_installed_plugin_packages(const boost::filesystem::path &data_directory,
                                         const PluginActivationConfig &config,
                                         std::string &error_message);

#ifdef SLIC3R_PLUGIN_REPOSITORY_TESTING
// The package-specific seam covers staging-copy failures. Publication and
// rollback tests use the neutral FilesystemTransaction seam.
enum class PluginPackageTransactionTestPoint {
    BeforeStageCopy
};

using PluginPackageTransactionTestHook = std::function<void(
    PluginPackageTransactionTestPoint, const std::string &)>;

void set_plugin_package_transaction_test_hook(PluginPackageTransactionTestHook hook);
#endif

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
