///|/ Copyright (c) SuperSlicer 2026 Durand Remi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/

// See UpdaterErrorMessages.hpp. Technical details stay available after the
// localized summary because they are useful when diagnosing a failed update.

#include "UpdaterErrorMessages.hpp"

#include "slic3r/GUI/I18N.hpp"

namespace Slic3r::GUI {

std::string updater_error_short_label(const UpdaterError &error)
{
    switch (error.code) {
    case UpdaterError::Code::None:                      return std::string();
    case UpdaterError::Code::RepositoryNotFound:        return _u8L("Repository unavailable");
    case UpdaterError::Code::ArchiveUnavailable:        return _u8L("Archive unavailable");
    case UpdaterError::Code::RateLimited:               return _u8L("Rate limit reached");
    case UpdaterError::Code::Network:                   return _u8L("Download failed");
    case UpdaterError::Code::Cache:                     return _u8L("Cache error");
    case UpdaterError::Code::InvalidRepositoryMetadata: return _u8L("Invalid repository data");
    case UpdaterError::Code::InvalidArchive:            return _u8L("Invalid archive");
    case UpdaterError::Code::Filesystem:                return _u8L("Filesystem error");
    case UpdaterError::Code::PreparationRejected:       return _u8L("Update cancelled");
    case UpdaterError::Code::Unexpected:                return _u8L("Unexpected updater error");
    }
    return _u8L("Update failed");
}

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
    case UpdaterError::Code::InvalidRepositoryMetadata:
        message = _u8L("The repository returned invalid update metadata.");
        break;
    case UpdaterError::Code::InvalidArchive:        message = _u8L("The downloaded update archive is invalid."); break;
    case UpdaterError::Code::Filesystem:            message = _u8L("The update could not be written to disk."); break;
    case UpdaterError::Code::PreparationRejected:   message = _u8L("The update was cancelled before files were changed."); break;
    case UpdaterError::Code::Unexpected:            message = _u8L("The updater encountered an unexpected error."); break;
    }

    if (!error.detail.empty())
        message += "\n\n" + error.detail;
    return message;
}

} // namespace Slic3r::GUI
