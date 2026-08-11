///|/ Copyright (c) SuperSlicer 2026 Durand Remi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/

// RepositoryPackageCache defines the on-disk package cache shared by vendor
// presets and external plugins. The generic class owns filesystem layout,
// archive extraction and atomic publication. A small adapter owns the details
// of each package type: where its payload lives, how metadata is discovered,
// and how a staged version is validated.
//
// Callers normally prepare_layout() once, then use cache_simple() for a bare
// local input or cache_archive() for a complete package. scan() returns only
// validated versions, so updater models never need to inspect directories or
// parse package manifests themselves.

#ifndef slic3r_Updater_RepositoryPackageCache_hpp_
#define slic3r_Updater_RepositoryPackageCache_hpp_

#include <optional>
#include <string>
#include <vector>

#include <boost/filesystem/path.hpp>

#include "libslic3r/Plugins/PluginRepository.hpp"

namespace Slic3r {

enum class RepositoryPackageSource {
    Simple,
    Package
};

// A download expectation is intentionally smaller than a repository
// description. It identifies the package selected by a repository tag without
// pretending that description.ini owns version information.
struct RepositoryPackageExpectation {
    RepositoryPackageType type = RepositoryPackageType::Plugin;
    std::string id;
    RepositoryPackageVersion version;
};

struct RepositoryCachedVersion {
    RepositoryDescription description;
    RepositoryPackageVersion version;
    boost::filesystem::path directory;
};

struct RepositoryCachedEntry {
    RepositoryDescription description;
    boost::filesystem::path directory;
    std::vector<RepositoryCachedVersion> versions;
};

// RepositoryPackageCacheAdapter is implemented once per payload family. Its
// methods never choose cache paths or replace existing versions; they operate
// only on a source or temporary staging directory owned by the generic cache.
class RepositoryPackageCacheAdapter {
public:
    virtual ~RepositoryPackageCacheAdapter() = default;

    virtual RepositoryPackageType package_type() const = 0;

    // Read generic identity and the independently stored package version.
    // expected is present for a remote archive whose repository tag already
    // declares both. Conflicting sources must be rejected.
    virtual bool inspect(const boost::filesystem::path &source,
                         RepositoryPackageSource source_type,
                         const std::optional<RepositoryPackageExpectation> &expected,
                         RepositoryDescription &description,
                         RepositoryPackageVersion &version,
                         std::string &error_message) const = 0;

    // Copy and normalize source into an empty staging directory. The resulting
    // directory is the exact version payload later moved into the cache.
    virtual bool stage(const boost::filesystem::path &source,
                       RepositoryPackageSource source_type,
                       const RepositoryDescription &description,
                       const RepositoryPackageVersion &version,
                       const boost::filesystem::path &staging,
                       std::string &error_message) const = 0;

    // Check the complete staged/cached payload, including its normalized
    // description.ini and mandatory type-specific files.
    virtual bool validate(const boost::filesystem::path &version_directory,
                          const RepositoryDescription &description,
                          const RepositoryPackageVersion &version,
                          std::string &error_message) const = 0;
};

class RepositoryPackageCache {
public:
    RepositoryPackageCache(boost::filesystem::path data_directory,
                           const RepositoryPackageCacheAdapter &adapter);

    // Create the current schema marker. If an older unmarked layout exists,
    // its type root is deleted once and purged is set to true so the caller can
    // repopulate built-in or currently installed content.
    bool prepare_layout(bool &purged, std::string &error_message) const;

    boost::filesystem::path type_directory() const;
    boost::filesystem::path repository_directory(const std::string &id) const;
    boost::filesystem::path repository_description_path(const std::string &id) const;
    boost::filesystem::path repository_tags_path(const std::string &id) const;
    boost::filesystem::path repository_logs_directory(const std::string &id) const;
    boost::filesystem::path version_directory(const std::string &id,
                                              const std::string &package_version,
                                              const std::string &slicer_version) const;

    // Save repository-level metadata such as a downloaded description.
    bool save_repository_description(const RepositoryDescription &description,
                                     std::string &error_message) const;

    // Import a bare vendor INI or plugin package directory. The adapter fills
    // missing metadata with the documented local-package defaults.
    bool cache_simple(const boost::filesystem::path &source,
                      RepositoryCachedVersion &cached,
                      std::string &error_message) const;

    // Import an archive. expected may identify the repository tag that caused
    // the download; local archive imports omit it and use package metadata.
    bool cache_archive(const boost::filesystem::path &archive_path,
                       const std::optional<RepositoryPackageExpectation> &expected,
                       RepositoryCachedVersion &cached,
                       std::string &error_message) const;

    // Import an already unpacked package, for example a live plugin copied back
    // into a freshly purged cache.
    bool cache_package_directory(const boost::filesystem::path &package_directory,
                                 const std::optional<RepositoryPackageExpectation> &expected,
                                 RepositoryCachedVersion &cached,
                                 std::string &error_message) const;

    // Discover root descriptors and validated version directories. Invalid
    // entries are skipped and reported through warnings by the implementation.
    std::vector<RepositoryCachedEntry> scan() const;

    static std::string safe_id(const std::string &id);
    static std::string version_directory_name(const std::string &package_version,
                                              const std::string &slicer_version);

private:
    bool cache_source(const boost::filesystem::path &source,
                      RepositoryPackageSource source_type,
                      const std::optional<RepositoryPackageExpectation> &expected,
                      RepositoryCachedVersion &cached,
                      std::string &error_message) const;
    bool publish(const boost::filesystem::path &source,
                 RepositoryPackageSource source_type,
                 const RepositoryDescription &description,
                 const RepositoryPackageVersion &version,
                 RepositoryCachedVersion &cached,
                 std::string &error_message) const;
    bool refresh_repository_description(const std::string &id,
                                        const RepositoryDescription &fallback,
                                        const RepositoryPackageVersion &fallback_version,
                                        std::string &error_message) const;

    boost::filesystem::path m_data_directory;
    const RepositoryPackageCacheAdapter &m_adapter;
};

// The process-wide adapters are stateless and may be shared by updaters,
// PluginLoader and tests.
const RepositoryPackageCacheAdapter &vendor_repository_cache_adapter();
const RepositoryPackageCacheAdapter &plugin_repository_cache_adapter();

} // namespace Slic3r

#endif // slic3r_Updater_RepositoryPackageCache_hpp_
