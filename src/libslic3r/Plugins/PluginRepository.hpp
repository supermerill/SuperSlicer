///|/ Copyright (c) SuperSlicer 2026 Durand Remi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/

#ifndef plugins_pluginrepository_hpp_
#define plugins_pluginrepository_hpp_

#include <map>
#include <string>

#include <boost/filesystem/path.hpp>

namespace Slic3r {

// The activation file has two independent concerns. A package version is
// selected in [installed], while individual plugin ids are enabled in
// [activated] after that package has been loaded on the next launch.
struct PluginActivationConfig {
    std::map<std::string, bool> activated;
    std::map<std::string, std::string> installed;
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
bool install_requested_plugin_packages(const boost::filesystem::path &data_directory,
                                       const PluginActivationConfig &config,
                                       std::string &error_message);

// Schedule a cached package version for installation on the next process
// start. The current process never replaces a loaded plugin library.
bool request_plugin_install(const std::string &package_name,
                            const std::string &version,
                            std::string &error_message);

} // namespace Slic3r

#endif // plugins_pluginrepository_hpp_
