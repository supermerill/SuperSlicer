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
        InvalidArchive,
        Filesystem,
        PreparationRejected
    };

    Code        code = Code::None;
    std::string detail;

    bool succeeded() const { return code == Code::None; }
};

using UpdaterErrors = std::vector<UpdaterError>;

} // namespace Slic3r

#endif // slic3r_Updater_UpdaterError_hpp_
