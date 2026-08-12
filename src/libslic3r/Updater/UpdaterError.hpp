///|/ Copyright (c) SuperSlicer 2026 Durand Remi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/

// Updaters run in console, server and GUI processes. This value type carries
// a stable machine-readable failure category plus optional technical detail;
// only the GUI turns it into localized text for a user.

#ifndef slic3r_Updater_UpdaterError_hpp_
#define slic3r_Updater_UpdaterError_hpp_

#include <string>
#include <utility>
#include <vector>

namespace Slic3r {

struct UpdaterError {
    enum class Code {
        None,
        RepositoryNotFound,
        ArchiveUnavailable,
        RateLimited,
        Network,
        Cache,
        InvalidRepositoryMetadata,
        InvalidArchive,
        Filesystem,
        PreparationRejected
    };

    Code        code = Code::None;
    std::string detail;

    bool succeeded() const { return code == Code::None; }
};

// Constructs the common updater result without making each repository engine
// repeat the same field assignments. An omitted detail is useful for stable
// error categories whose user-facing text is supplied by the GUI.
inline UpdaterError make_updater_error(UpdaterError::Code code, std::string detail = std::string())
{
    UpdaterError error;
    error.code = code;
    error.detail = std::move(detail);
    return error;
}

using UpdaterErrors = std::vector<UpdaterError>;

} // namespace Slic3r

#endif // slic3r_Updater_UpdaterError_hpp_
