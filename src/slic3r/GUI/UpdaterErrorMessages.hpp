///|/ Copyright (c) SuperSlicer 2026 Durand Remi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/

// This GUI-only formatter owns every user-facing updater message. Core
// updaters return UpdaterError so they remain usable without wxWidgets.

#ifndef slic3r_GUI_UpdaterErrorMessages_hpp_
#define slic3r_GUI_UpdaterErrorMessages_hpp_

#include <string>

#include "libslic3r/Updater/UpdaterError.hpp"

namespace Slic3r::GUI {

// Returns the short status used in compact controls such as an updater table.
// Call format_updater_error() for the explanatory tooltip or error dialog.
std::string updater_error_short_label(const UpdaterError &error);

std::string format_updater_error(const UpdaterError &error);

} // namespace Slic3r::GUI

#endif // slic3r_GUI_UpdaterErrorMessages_hpp_
