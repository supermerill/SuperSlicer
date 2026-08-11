///|/ Copyright (c) SuperSlicer 2026 Durand Remi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/

#ifndef slic3r_HttpErrorMessages_hpp_
#define slic3r_HttpErrorMessages_hpp_

#include <string>

#include "libslic3r/Updater/Http.hpp"

namespace Slic3r {
namespace GUI {

// Formats raw transport diagnostics for the interactive GUI. Keeping this
// separate prevents the reusable HTTP layer from depending on wxWidgets or a
// particular translation domain.
std::string format_tls_initialization_message(const Http::TlsInitializationResult &result);

} // namespace GUI
} // namespace Slic3r

#endif
