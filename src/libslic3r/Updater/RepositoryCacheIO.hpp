///|/ Copyright (c) SuperSlicer 2026 Durand Remi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher

// RepositoryCacheIO contains the filesystem operations shared by repository
// metadata services. Callers validate downloaded contents before publication;
// this module only guarantees complete reads, checked writes and atomic cache
// replacement with restoration of the previous file when publication fails.

#ifndef slic3r_Updater_RepositoryCacheIO_hpp_
#define slic3r_Updater_RepositoryCacheIO_hpp_

#include <string>

#include <boost/filesystem/path.hpp>

#include "libslic3r/Updater/UpdaterError.hpp"

namespace Slic3r {
namespace RepositoryUpdaterInternal {

// Reads one complete cache file. A missing file is a successful result with
// exists set to false; local access or stream failures return Filesystem.
UpdaterError read_repository_cache_file(const boost::filesystem::path &path,
                                        std::string &contents,
                                        bool &exists);

// Writes one complete file after creating its parent directory. This primitive
// is intended for new destinations; use publish_repository_cache_atomically()
// when replacing metadata that must survive an interrupted update.
UpdaterError write_repository_file(const boost::filesystem::path &destination,
                                   const std::string &contents);

// Publishes validated metadata through a sibling temporary file. The previous
// cache is restored if the final rename fails, and temporary files are removed
// on both successful and failed paths.
UpdaterError publish_repository_cache_atomically(const boost::filesystem::path &destination,
                                                  const std::string &contents);

} // namespace RepositoryUpdaterInternal
} // namespace Slic3r

#endif // slic3r_Updater_RepositoryCacheIO_hpp_
