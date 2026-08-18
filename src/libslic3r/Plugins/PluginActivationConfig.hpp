///|/ Copyright (c) SuperSlicer 2026 Durand Remi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/

// This module defines and persists plugins/activated.ini. The file records the
// complete desired package set, individual plugin activation choices, and the
// package which provides each external plugin id. Callers which may rewrite the
// file use the strict reader; startup may use the tolerant reader to retain
// valid entries while presenting every rejected entry to the user.

#ifndef plugins_pluginactivationconfig_hpp_
#define plugins_pluginactivationconfig_hpp_

#include <initializer_list>
#include <map>
#include <string>
#include <vector>

#include <boost/filesystem/path.hpp>

namespace Slic3r {

struct PluginInstalledVersion {
    std::string package_version;
    std::string slicer_version;
};

// [installed] is the authoritative desired package set. Plugin ids remain
// independently enabled in [activated], while [plugin_packages] lets the GUI
// preserve and diagnose unavailable external ids.
struct PluginActivationConfig {
    std::map<std::string, bool> activated;
    std::map<std::string, std::string> plugin_packages;
    std::map<std::string, PluginInstalledVersion> installed;
};

enum class PluginActivationConfigStatus {
    Valid,
    PartiallyValid,
    Invalid
};

// An empty key denotes an issue affecting a complete section rather than one
// assignment. Keeping these fields separate lets the GUI format useful repair
// information without parsing a flattened error message.
struct PluginActivationConfigIssue {
    std::string section;
    std::string key;
    std::string reason;
};

// rejected_packages contains only safe ids whose invalid desired state also
// removes known provider associations and activations from config.
struct PluginActivationConfigReadResult {
    PluginActivationConfigStatus status = PluginActivationConfigStatus::Invalid;
    PluginActivationConfig config;
    std::vector<PluginActivationConfigIssue> issues;
    std::vector<std::string> rejected_packages;
    std::string error_message;
};

// Package paths and versions are validated with the same rules while parsing
// activated.ini, importing archives, reconciling live packages, and scheduling
// removals. These helpers are public to keep that security boundary unique.
bool is_valid_plugin_package_name(const std::string &package_name);
bool is_valid_plugin_package_version(const std::string &version);

// Replace one obsolete activation id with independent successor ids. Existing
// successor choices are authoritative, while missing choices inherit the old
// enabled state. The obsolete provider association is removed as well.
bool migrate_plugin_activation_id(PluginActivationConfig &config,
                                  const std::string &obsolete_id,
                                  std::initializer_list<std::string> replacement_ids);

// Return data_dir/plugins/activated.ini, the durable desired state consumed at
// process startup and updated by package and activation dialogs.
boost::filesystem::path plugin_activation_config_path(const boost::filesystem::path &data_directory);

// The tolerant reader returns every valid entry plus structured diagnostics.
// The strict reader rejects any semantic issue and is required before a caller
// modifies and republishes the file. [installed] is mandatory but may be empty.
PluginActivationConfigReadResult read_plugin_activation_config_tolerant(
    const boost::filesystem::path &config_path);
bool read_plugin_activation_config(const boost::filesystem::path &config_path,
                                   PluginActivationConfig &config,
                                   std::string &error_message);

// Write a complete sibling staging file, then publish it through the shared
// filesystem transaction engine. A failed publication therefore leaves the
// original configuration intact or reports an incomplete rollback.
bool write_plugin_activation_config(const boost::filesystem::path &config_path,
                                    const PluginActivationConfig &config,
                                    std::string &error_message);

// Validate and atomically copy the complete resource default over a damaged
// user file. Package reconciliation is deliberately deferred until restart.
bool replace_plugin_activation_config_with_defaults(const boost::filesystem::path &config_path,
                                                    std::string &error_message);

// Create the user file atomically from resources when absent, then read it
// strictly. An empty data directory selects the resource default directly.
bool ensure_plugin_activation_config(const boost::filesystem::path &data_directory,
                                     PluginActivationConfig &config,
                                     bool &from_user_config,
                                     std::string &error_message);

} // namespace Slic3r

#endif // plugins_pluginactivationconfig_hpp_
