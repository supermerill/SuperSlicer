///|/ Copyright (c) SuperSlicer 2026 Durand Remi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/

// See UpdaterErrorMessages.hpp. Technical details stay available after the
// localized summary because they are useful when diagnosing a failed update.

#include "UpdaterErrorMessages.hpp"

#include "slic3r/GUI/I18N.hpp"

namespace Slic3r::GUI {

std::string format_updater_error(const UpdaterError &error)
{
    std::string message;
    switch (error.code) {
    case UpdaterError::Code::None:                  return message;
    case UpdaterError::Code::RepositoryNotFound:    message = _u8L("The requested repository was not found."); break;
    case UpdaterError::Code::ArchiveUnavailable:    message = _u8L("The selected version has no downloadable archive."); break;
    case UpdaterError::Code::RateLimited:           message = _u8L("Too many requests to GitHub. Please try again later."); break;
    case UpdaterError::Code::Network:               message = _u8L("The update download failed."); break;
    case UpdaterError::Code::Cache:                 message = _u8L("The downloaded update could not be cached."); break;
    case UpdaterError::Code::InvalidArchive:        message = _u8L("The downloaded update archive is invalid."); break;
    case UpdaterError::Code::Filesystem:            message = _u8L("The update could not be written to disk."); break;
    case UpdaterError::Code::PreparationRejected:   message = _u8L("The update was cancelled before files were changed."); break;
    }

    if (!error.detail.empty())
        message += "\n\n" + error.detail;
    return message;
}

} // namespace Slic3r::GUI
