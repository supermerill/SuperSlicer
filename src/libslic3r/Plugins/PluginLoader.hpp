///|/ Copyright (c) SuperSlicer 2026 Durand Rémi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/

#ifndef plugins_pluginloader_hpp_
#define plugins_pluginloader_hpp_

namespace boost {
namespace filesystem {
class path;
}
}

namespace Slic3r {

class Orchestrator;

// Load every installed package found directly below repository and retain one
// PluginPackageLoadReport per attempted package in orchestrator. The normal
// startup uses this after applying package changes; tests and small host tools
// may use it to validate an isolated repository.
void load_plugin_packages_from_repository(const boost::filesystem::path &repository,
                                          Orchestrator &orchestrator);

// Register built-in plugins, load plugin libraries from the runtime plugin
// repository, activate the configured subset, then initialize active plugins.
void load_plugins();

// Register selector settings for active exclusive plugin groups.
//
// This runs before plugin initialization so plugins may refer to the selector
// setting from their own UI fragments. The matching UI fallback is registered
// later, after plugins had a chance to provide a group-specific fragment.
void register_exclusive_step_group_options(Orchestrator &orchestrator);

// Register fallback UI fragments for active exclusive plugin groups.
//
// The fragment id is always the exclusive group id. If a plugin already
// registered a better placed fragment with that id, the fallback is ignored by
// Orchestrator's normal fragment de-duplication.
void register_exclusive_step_group_ui_fragments(Orchestrator &orchestrator);

// Compatibility helper for tests or small tools that do not need to inject
// custom fragments between option creation and fallback registration.
void register_exclusive_step_groups(Orchestrator &orchestrator);

} // namespace Slic3r

#endif // plugins_pluginloader_hpp_
