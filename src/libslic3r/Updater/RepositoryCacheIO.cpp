///|/ Copyright (c) SuperSlicer 2026 Durand Remi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher

// See RepositoryCacheIO.hpp. Cache writers first finish a sibling staging
// file, then temporarily move the previous destination aside. This ordering is
// required on Windows, where rename cannot replace an existing file directly.

#include "libslic3r/Updater/RepositoryCacheIO.hpp"

#include <exception>
#include <iterator>
#include <utility>

#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>
#include <boost/nowide/fstream.hpp>

namespace Slic3r {
namespace RepositoryUpdaterInternal {

UpdaterError read_repository_cache_file(const boost::filesystem::path &path,
                                        std::string &contents,
                                        bool &exists)
{
    try {
        exists = boost::filesystem::is_regular_file(path);
        contents.clear();
        if (!exists)
            return UpdaterError();

        boost::nowide::ifstream stream(path.string(), std::ios::in | std::ios::binary);
        if (!stream)
            return make_updater_error(UpdaterError::Code::Filesystem,
                                      "Cannot read repository cache '" + path.string() + "'.");
        contents.assign(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
        if (stream.bad())
            return make_updater_error(UpdaterError::Code::Filesystem,
                                      "Cannot finish reading repository cache '" + path.string() + "'.");
        return UpdaterError();
    } catch (const std::exception &error) {
        return make_updater_error(UpdaterError::Code::Filesystem, error.what());
    }
}

UpdaterError write_repository_file(const boost::filesystem::path &destination,
                                   const std::string &contents)
{
    try {
        if (!destination.parent_path().empty())
            boost::filesystem::create_directories(destination.parent_path());

        boost::nowide::ofstream stream(destination.string(), std::ios::out | std::ios::binary | std::ios::trunc);
        if (!stream)
            return make_updater_error(UpdaterError::Code::Filesystem,
                                      "Cannot create the downloaded repository file.");
        stream.write(contents.data(), static_cast<std::streamsize>(contents.size()));
        if (!stream)
            return make_updater_error(UpdaterError::Code::Filesystem,
                                      "Cannot write the downloaded repository file.");
        stream.flush();
        if (!stream)
            return make_updater_error(UpdaterError::Code::Filesystem,
                                      "Cannot flush the downloaded repository file.");
        stream.close();
        if (!stream)
            return make_updater_error(UpdaterError::Code::Filesystem,
                                      "Cannot close the downloaded repository file.");
        return UpdaterError();
    } catch (const std::exception &error) {
        return make_updater_error(UpdaterError::Code::Filesystem, error.what());
    }
}

UpdaterError publish_repository_cache_atomically(const boost::filesystem::path &destination,
                                                  const std::string &contents)
{
    const boost::filesystem::path parent = destination.parent_path();
    const std::string filename = destination.filename().string();
    const boost::filesystem::path staging = parent /
        boost::filesystem::unique_path("." + filename + ".download-%%%%-%%%%");
    const boost::filesystem::path backup = parent /
        boost::filesystem::unique_path("." + filename + ".previous-%%%%-%%%%");
    bool previous_moved = false;

    try {
        if (!parent.empty())
            boost::filesystem::create_directories(parent);

        const UpdaterError write_error = write_repository_file(staging, contents);
        if (!write_error.succeeded()) {
            boost::system::error_code cleanup_error;
            boost::filesystem::remove(staging, cleanup_error);
            return write_error;
        }

        // Windows cannot rename over an existing destination. Keep the old
        // cache beside the staging file until the new file is in place.
        if (boost::filesystem::exists(destination)) {
            if (!boost::filesystem::is_regular_file(destination)) {
                boost::system::error_code cleanup_error;
                boost::filesystem::remove(staging, cleanup_error);
                return make_updater_error(UpdaterError::Code::Filesystem,
                                          "The repository cache destination is not a regular file.");
            }
            boost::filesystem::rename(destination, backup);
            previous_moved = true;
        }

        try {
            boost::filesystem::rename(staging, destination);
        } catch (const boost::filesystem::filesystem_error &error) {
            std::string detail = error.what();
            if (previous_moved && !boost::filesystem::exists(destination)) {
                boost::system::error_code restore_error;
                boost::filesystem::rename(backup, destination, restore_error);
                if (restore_error)
                    detail += "; restoring the previous cache also failed: " + restore_error.message();
            }
            boost::system::error_code cleanup_error;
            boost::filesystem::remove(staging, cleanup_error);
            return make_updater_error(UpdaterError::Code::Filesystem, std::move(detail));
        }

        if (previous_moved) {
            boost::system::error_code cleanup_error;
            boost::filesystem::remove(backup, cleanup_error);
            if (cleanup_error)
                BOOST_LOG_TRIVIAL(warning) << "Cannot remove previous repository cache '"
                                           << backup.string() << "': " << cleanup_error.message();
        }
        return UpdaterError();
    } catch (const std::exception &error) {
        boost::system::error_code cleanup_error;
        boost::filesystem::remove(staging, cleanup_error);
        if (previous_moved && !boost::filesystem::exists(destination)) {
            boost::system::error_code restore_error;
            boost::filesystem::rename(backup, destination, restore_error);
            if (restore_error)
                return make_updater_error(UpdaterError::Code::Filesystem,
                    std::string(error.what()) + "; restoring the previous cache also failed: " +
                    restore_error.message());
        }
        return make_updater_error(UpdaterError::Code::Filesystem, error.what());
    }
}

} // namespace RepositoryUpdaterInternal
} // namespace Slic3r
