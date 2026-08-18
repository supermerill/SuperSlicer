///|/ Copyright (c) SuperSlicer 2026 Durand Remi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher

// See RepositoryCacheIO.hpp. Cache writers finish every sibling staging file
// before the shared transaction changes any visible cache destination.

#include "libslic3r/Updater/RepositoryCacheIO.hpp"

#include <exception>
#include <iterator>
#include <utility>

#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>
#include <boost/nowide/fstream.hpp>

#include "libslic3r/FilesystemTransaction.hpp"

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
    return publish_repository_caches_atomically({RepositoryCachePublication{destination, contents}});
}

UpdaterError publish_repository_caches_atomically(
    const std::vector<RepositoryCachePublication> &publications)
{
    try {
        FilesystemTransaction transaction;

        // Register ownership before writing so a failed write cannot leave a
        // partially prepared cache beside its destination.
        for (const RepositoryCachePublication &publication : publications) {
            if (boost::filesystem::exists(publication.destination) &&
                !boost::filesystem::is_regular_file(publication.destination))
                return make_updater_error(
                    UpdaterError::Code::Filesystem,
                    "Repository cache destination '" + publication.destination.string() +
                    "' is not a regular file.");
            const boost::filesystem::path parent = publication.destination.parent_path();
            const std::string filename = publication.destination.filename().string();
            const boost::filesystem::path staging = parent /
                boost::filesystem::unique_path("." + filename + ".download-%%%%-%%%%");
            transaction.add_replacement(staging, publication.destination);
            const UpdaterError write_error = write_repository_file(staging, publication.contents);
            if (!write_error.succeeded())
                return write_error;
        }

        const FilesystemTransactionResult result = transaction.commit();
        for (const FilesystemTransactionFailure &warning : result.cleanup_warnings)
            BOOST_LOG_TRIVIAL(warning) << "Repository cache transaction cleanup warning: "
                                       << format_filesystem_transaction_failure(warning);
        if (result.status != FilesystemTransactionStatus::Committed)
            return make_updater_error(UpdaterError::Code::Filesystem,
                                      format_filesystem_transaction_error(result));
        return UpdaterError();
    } catch (const std::exception &error) {
        return make_updater_error(UpdaterError::Code::Filesystem, error.what());
    }
}

} // namespace RepositoryUpdaterInternal
} // namespace Slic3r
