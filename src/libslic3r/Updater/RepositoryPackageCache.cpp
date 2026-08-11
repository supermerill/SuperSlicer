///|/ Copyright (c) SuperSlicer 2026 Durand Remi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/

// See RepositoryPackageCache.hpp. This implementation keeps every filesystem
// mutation behind staging directories so a failed import cannot damage a
// previously validated version.

#include "libslic3r/Updater/RepositoryPackageCache.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iterator>
#include <sstream>
#include <utility>

#include <boost/algorithm/string/predicate.hpp>
#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>
#include <boost/nowide/fstream.hpp>
#include <boost/property_tree/ini_parser.hpp>

#include "libslic3r/Preset.hpp"
#include "libslic3r/Utils.hpp"
#include "libslic3r/libslic3r.h"

namespace Slic3r {
namespace {

const char *const CACHE_LAYOUT_VERSION = "2";
const char *const CACHE_LAYOUT_FILENAME = ".layout_version";
const char *const DESCRIPTION_FILENAME = "description.ini";
const char *const DEFAULT_VERSION = "1.0.0.0";

std::string package_type_directory(RepositoryPackageType type);
std::string package_section_name(RepositoryPackageType type);
std::string plugin_library_filename();
bool read_file(const boost::filesystem::path &path, std::string &contents);
bool repository_root_accepts_id(const boost::filesystem::path &descriptor,
                                RepositoryPackageType type,
                                const std::string &id,
                                std::string &error_message);
bool write_description_file(const boost::filesystem::path &path,
                            const RepositoryDescription &description,
                            bool include_version,
                            std::string &error_message);
bool copy_directory_tree(const boost::filesystem::path &source,
                         const boost::filesystem::path &destination,
                         std::string &error_message);
bool descriptions_match(const RepositoryDescription &actual,
                        const RepositoryDescription &expected,
                        std::string &error_message);
void apply_description_defaults(RepositoryDescription &description);
bool normalize_vendor_profile(const boost::filesystem::path &source,
                              const boost::filesystem::path &destination,
                              RepositoryDescription &description,
                              std::string &error_message);
bool locate_vendor_profile(const boost::filesystem::path &source,
                           RepositoryPackageSource source_type,
                           boost::filesystem::path &profile_path,
                           std::string &error_message);
bool parse_version_directory_name(const std::string &name,
                                  std::string &package_version,
                                  std::string &slicer_version);
bool newer_description(const RepositoryDescription &left, const RepositoryDescription &right);

class VendorRepositoryPackageCacheAdapter final : public RepositoryPackageCacheAdapter {
public:
    RepositoryPackageType package_type() const override { return RepositoryPackageType::Vendor; }
    bool inspect(const boost::filesystem::path &source,
                 RepositoryPackageSource source_type,
                 const std::optional<RepositoryDescription> &expected,
                 RepositoryDescription &description,
                 std::string &error_message) const override;
    bool stage(const boost::filesystem::path &source,
               RepositoryPackageSource source_type,
               const RepositoryDescription &description,
               const boost::filesystem::path &staging,
               std::string &error_message) const override;
    bool validate(const boost::filesystem::path &version_directory,
                  const RepositoryDescription &description,
                  std::string &error_message) const override;
};

class PluginRepositoryPackageCacheAdapter final : public RepositoryPackageCacheAdapter {
public:
    RepositoryPackageType package_type() const override { return RepositoryPackageType::Plugin; }
    bool inspect(const boost::filesystem::path &source,
                 RepositoryPackageSource source_type,
                 const std::optional<RepositoryDescription> &expected,
                 RepositoryDescription &description,
                 std::string &error_message) const override;
    bool stage(const boost::filesystem::path &source,
               RepositoryPackageSource source_type,
               const RepositoryDescription &description,
               const boost::filesystem::path &staging,
               std::string &error_message) const override;
    bool validate(const boost::filesystem::path &version_directory,
                  const RepositoryDescription &description,
                  std::string &error_message) const override;
};

std::string package_type_directory(RepositoryPackageType type)
{
    return type == RepositoryPackageType::Vendor ? "vendor" : "plugins";
}

std::string package_section_name(RepositoryPackageType type)
{
    return type == RepositoryPackageType::Vendor ? "vendor" : "plugin";
}

std::string plugin_library_filename()
{
#ifdef _WIN32
    return "plugin.dll";
#elif defined(__APPLE__)
    return "plugin.dylib";
#else
    return "plugin.so";
#endif
}

bool read_file(const boost::filesystem::path &path, std::string &contents)
{
    boost::nowide::ifstream stream(path.string(), std::ios::in | std::ios::binary);
    if (!stream)
        return false;
    contents.assign(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
    return true;
}

// A sanitized directory may represent only one raw repository id. Check its
// descriptor before any version is staged so colliding ids cannot replace one
// another even when they use the same version numbers.
bool repository_root_accepts_id(const boost::filesystem::path &descriptor,
                                RepositoryPackageType type,
                                const std::string &id,
                                std::string &error_message)
{
    if (!boost::filesystem::is_regular_file(descriptor))
        return true;

    std::string contents;
    RepositoryDescription existing;
    if (!read_file(descriptor, contents) ||
        !parse_repository_description(contents, type, existing, error_message)) {
        if (error_message.empty())
            error_message = "Cannot read existing repository description '" + descriptor.string() + "'.";
        return false;
    }
    if (existing.id == id)
        return true;
    error_message = "Repository ids '" + existing.id + "' and '" + id +
                    "' map to the same cache directory.";
    return false;
}

// Root descriptors intentionally omit versions. Version descriptions use the
// key expected by their package family while sharing all identity fields.
bool write_description_file(const boost::filesystem::path &path,
                            const RepositoryDescription &description,
                            bool include_version,
                            std::string &error_message)
{
    try {
        boost::filesystem::create_directories(path.parent_path());
        boost::nowide::ofstream stream(path.string(), std::ios::out | std::ios::trunc);
        if (!stream) {
            error_message = "Cannot create repository description '" + path.string() + "'.";
            return false;
        }

        stream << '[' << package_section_name(description.type) << "]\n";
        stream << "id = " << description.id << "\n";
        stream << "name = " << description.name << "\n";
        stream << "full_name = " << description.full_name << "\n";
        stream << "description = " << description.description << "\n";
        stream << "config_update_rest = " << description.config_update_rest << "\n";
        stream << "slicer = " << description.slicer << "\n";
        if (include_version) {
            stream << (description.type == RepositoryPackageType::Vendor ? "config_version = " : "package_version = ")
                   << description.package_version << "\n";
            stream << "slicer_version = " << description.slicer_version << "\n";
        }
        if (!stream.good()) {
            error_message = "Cannot finish repository description '" + path.string() + "'.";
            return false;
        }
        return true;
    } catch (const boost::filesystem::filesystem_error &error) {
        error_message = error.what();
        return false;
    }
}

// Copy the complete package tree without following ownership outside source.
bool copy_directory_tree(const boost::filesystem::path &source,
                         const boost::filesystem::path &destination,
                         std::string &error_message)
{
    try {
        if (!boost::filesystem::is_directory(source)) {
            error_message = "Package source '" + source.string() + "' is not a directory.";
            return false;
        }
        boost::filesystem::create_directories(destination);
        for (boost::filesystem::recursive_directory_iterator it(source), end; it != end; ++it) {
            const boost::filesystem::path relative = boost::filesystem::relative(it->path(), source);
            const boost::filesystem::path output = destination / relative;
            if (boost::filesystem::is_directory(it->path()))
                boost::filesystem::create_directories(output);
            else if (boost::filesystem::is_regular_file(it->path())) {
                boost::filesystem::create_directories(output.parent_path());
                boost::filesystem::copy_file(it->path(), output, boost::filesystem::copy_option::overwrite_if_exists);
            }
        }
        return true;
    } catch (const boost::filesystem::filesystem_error &error) {
        error_message = error.what();
        return false;
    }
}

bool descriptions_match(const RepositoryDescription &actual,
                        const RepositoryDescription &expected,
                        std::string &error_message)
{
    if (!expected.id.empty() && actual.id != expected.id) {
        error_message = "Package id '" + actual.id + "' does not match expected id '" + expected.id + "'.";
        return false;
    }
    if (!expected.package_version.empty() && actual.package_version != expected.package_version) {
        error_message = "Package version '" + actual.package_version + "' does not match expected version '" +
                        expected.package_version + "'.";
        return false;
    }
    if (!expected.slicer_version.empty() && actual.slicer_version != expected.slicer_version) {
        error_message = "Package slicer version '" + actual.slicer_version + "' does not match expected version '" +
                        expected.slicer_version + "'.";
        return false;
    }
    return true;
}

void apply_description_defaults(RepositoryDescription &description)
{
    if (description.package_version.empty())
        description.package_version = DEFAULT_VERSION;
    if (description.slicer_version.empty())
        description.slicer_version = DEFAULT_VERSION;
    if (description.name.empty())
        description.name = description.id;
    if (description.full_name.empty())
        description.full_name = description.name;
    if (description.slicer.empty())
        description.slicer = SLIC3R_APP_KEY;
}

// Rewrite the vendor header in the staged copy so defaults are part of the
// installable profile itself, not only its generated description.ini.
bool normalize_vendor_profile(const boost::filesystem::path &source,
                              const boost::filesystem::path &destination,
                              RepositoryDescription &description,
                              std::string &error_message)
{
    try {
        boost::property_tree::ptree tree;
        boost::property_tree::read_ini(source.string(), tree);
        boost::property_tree::ptree &vendor = tree.get_child("vendor");
        const std::string fallback_id = source.stem().string();
        description = {};
        description.type = RepositoryPackageType::Vendor;
        description.id = vendor.get<std::string>("id", fallback_id);
        description.name = vendor.get<std::string>("name", description.id);
        description.full_name = vendor.get<std::string>("full_name", description.name);
        description.description = vendor.get<std::string>("description", std::string());
        description.config_update_rest = vendor.get<std::string>("config_update_rest", std::string());
        description.slicer = vendor.get<std::string>("slicer", std::string());
        description.package_version = vendor.get<std::string>("config_version", std::string());
        description.slicer_version = vendor.get<std::string>("slicer_version", std::string());
        apply_description_defaults(description);

        vendor.put("id", description.id);
        vendor.put("name", description.name);
        vendor.put("full_name", description.full_name);
        vendor.put("description", description.description);
        vendor.put("config_update_rest", description.config_update_rest);
        vendor.put("slicer", description.slicer);
        vendor.put("config_version", description.package_version);
        vendor.put("slicer_version", description.slicer_version);
        boost::filesystem::create_directories(destination.parent_path());
        boost::property_tree::write_ini(destination.string(), tree);
        return true;
    } catch (const std::exception &error) {
        error_message = "Cannot normalize vendor profile '" + source.string() + "': " + error.what();
        return false;
    }
}

bool locate_vendor_profile(const boost::filesystem::path &source,
                           RepositoryPackageSource source_type,
                           boost::filesystem::path &profile_path,
                           std::string &error_message)
{
    if (source_type == RepositoryPackageSource::Simple) {
        if (!boost::filesystem::is_regular_file(source)) {
            error_message = "The vendor source is not an INI file.";
            return false;
        }
        profile_path = source;
        return true;
    }

    const boost::filesystem::path profiles = source / "profiles";
    if (!boost::filesystem::is_directory(profiles)) {
        error_message = "The vendor package has no profiles directory.";
        return false;
    }
    for (boost::filesystem::directory_iterator it(profiles), end; it != end; ++it) {
        if (!boost::filesystem::is_regular_file(it->path()) || it->path().extension() != ".ini")
            continue;
        if (!profile_path.empty()) {
            error_message = "A vendor package must contain exactly one profile INI.";
            return false;
        }
        profile_path = it->path();
    }
    if (profile_path.empty()) {
        error_message = "The vendor package contains no profile INI.";
        return false;
    }
    return true;
}

bool parse_version_directory_name(const std::string &name,
                                  std::string &package_version,
                                  std::string &slicer_version)
{
    const size_t separator = name.find('=');
    if (separator == std::string::npos || separator == 0 || separator + 1 == name.size() ||
        name.find('=', separator + 1) != std::string::npos)
        return false;
    package_version = name.substr(0, separator);
    slicer_version = name.substr(separator + 1);
    return Semver::parse(package_version).has_value() && Semver::parse(slicer_version).has_value();
}

bool newer_description(const RepositoryDescription &left, const RepositoryDescription &right)
{
    const std::optional<Semver> left_package = Semver::parse(left.package_version);
    const std::optional<Semver> right_package = Semver::parse(right.package_version);
    if (!left_package || !right_package)
        return false;
    if (*left_package != *right_package)
        return *left_package > *right_package;
    return *Semver::parse(left.slicer_version) > *Semver::parse(right.slicer_version);
}

bool VendorRepositoryPackageCacheAdapter::inspect(
    const boost::filesystem::path &source,
    RepositoryPackageSource source_type,
    const std::optional<RepositoryDescription> &expected,
    RepositoryDescription &description,
    std::string &error_message) const
{
    boost::filesystem::path profile_path;
    if (!locate_vendor_profile(source, source_type, profile_path, error_message))
        return false;

    const boost::filesystem::path temporary = boost::filesystem::temp_directory_path() /
        boost::filesystem::unique_path(".vendor-description-%%%%-%%%%.ini");
    const bool normalized = normalize_vendor_profile(profile_path, temporary, description, error_message);
    boost::system::error_code ignored_error;
    boost::filesystem::remove(temporary, ignored_error);
    if (!normalized)
        return false;

    // Complete archives may repeat package metadata in description.ini. It is
    // not used to relabel the vendor profile, but conflicting identity or
    // versions make the archive ambiguous and are rejected.
    const boost::filesystem::path manifest_path = source / DESCRIPTION_FILENAME;
    if (source_type == RepositoryPackageSource::Package &&
        boost::filesystem::is_regular_file(manifest_path)) {
        std::string contents;
        RepositoryDescription manifest;
        if (!read_file(manifest_path, contents) ||
            !parse_repository_description(contents, RepositoryPackageType::Vendor, manifest, error_message))
            return false;
        apply_description_defaults(manifest);
        if (!descriptions_match(description, manifest, error_message))
            return false;
    }
    return !expected || descriptions_match(description, *expected, error_message);
}

bool VendorRepositoryPackageCacheAdapter::stage(
    const boost::filesystem::path &source,
    RepositoryPackageSource source_type,
    const RepositoryDescription &description,
    const boost::filesystem::path &staging,
    std::string &error_message) const
{
    boost::filesystem::path profile_path;
    if (!locate_vendor_profile(source, source_type, profile_path, error_message))
        return false;

    try {
        if (source_type == RepositoryPackageSource::Package &&
            !copy_directory_tree(source, staging, error_message))
            return false;
        boost::filesystem::create_directories(staging / "profiles");

        RepositoryDescription normalized;
        const boost::filesystem::path normalized_profile = staging / "profiles" / (description.id + ".ini");
        if (!normalize_vendor_profile(profile_path, normalized_profile,
                                      normalized, error_message))
            return false;
        if (!descriptions_match(normalized, description, error_message))
            return false;

        // A package may name its input profile after an archive wrapper. Keep
        // one canonical profile named after the raw vendor id in the cached
        // payload so discovery cannot observe two competing INI files.
        if (source_type == RepositoryPackageSource::Package) {
            const boost::filesystem::path copied_profile = staging / "profiles" / profile_path.filename();
            if (copied_profile != normalized_profile)
                boost::filesystem::remove(copied_profile);
        }

        // A bare profile may have a sibling icon directory named after its id.
        if (source_type == RepositoryPackageSource::Simple) {
            const boost::filesystem::path icons = source.parent_path() / description.id;
            if (boost::filesystem::is_directory(icons) &&
                !copy_directory_tree(icons, staging / "profiles" / description.id, error_message))
                return false;
        }
        return write_description_file(staging / DESCRIPTION_FILENAME, description, true, error_message);
    } catch (const boost::filesystem::filesystem_error &error) {
        error_message = error.what();
        return false;
    }
}

bool VendorRepositoryPackageCacheAdapter::validate(
    const boost::filesystem::path &version_directory,
    const RepositoryDescription &description,
    std::string &error_message) const
{
    const boost::filesystem::path profile_path = version_directory / "profiles" / (description.id + ".ini");
    if (!boost::filesystem::is_regular_file(profile_path)) {
        error_message = "Vendor version has no profile '" + profile_path.string() + "'.";
        return false;
    }
    try {
        const VendorProfile profile = VendorProfile::from_ini(profile_path, true);
        if (profile.id != description.id || profile.config_version.to_string() != description.package_version ||
            profile.slicer_version.to_string() != description.slicer_version) {
            error_message = "Vendor profile metadata does not match its version description.";
            return false;
        }
    } catch (const std::exception &error) {
        error_message = error.what();
        return false;
    }
    return true;
}

bool PluginRepositoryPackageCacheAdapter::inspect(
    const boost::filesystem::path &source,
    RepositoryPackageSource,
    const std::optional<RepositoryDescription> &expected,
    RepositoryDescription &description,
    std::string &error_message) const
{
    if (!boost::filesystem::is_directory(source) ||
        !boost::filesystem::is_regular_file(source / plugin_library_filename())) {
        error_message = "Plugin package must be a directory containing '" + plugin_library_filename() + "'.";
        return false;
    }

    const boost::filesystem::path description_path = source / DESCRIPTION_FILENAME;
    if (boost::filesystem::is_regular_file(description_path)) {
        std::string contents;
        if (!read_file(description_path, contents) ||
            !parse_repository_description(contents, RepositoryPackageType::Plugin, description, error_message))
            return false;
    } else {
        // A live plugin copied back after a layout purge may predate package
        // descriptions. Its requested activation version is authoritative in
        // that case; a manually imported folder still derives its identity
        // from the directory name and receives local-package defaults.
        description = expected.value_or(RepositoryDescription{});
        description.type = RepositoryPackageType::Plugin;
        if (description.id.empty())
            description.id = source.filename().string();
    }
    apply_description_defaults(description);
    return !expected || descriptions_match(description, *expected, error_message);
}

bool PluginRepositoryPackageCacheAdapter::stage(
    const boost::filesystem::path &source,
    RepositoryPackageSource,
    const RepositoryDescription &description,
    const boost::filesystem::path &staging,
    std::string &error_message) const
{
    if (!copy_directory_tree(source, staging, error_message))
        return false;
    return write_description_file(staging / DESCRIPTION_FILENAME, description, true, error_message);
}

bool PluginRepositoryPackageCacheAdapter::validate(
    const boost::filesystem::path &version_directory,
    const RepositoryDescription &description,
    std::string &error_message) const
{
    if (!boost::filesystem::is_regular_file(version_directory / plugin_library_filename())) {
        error_message = "Plugin version does not contain '" + plugin_library_filename() + "'.";
        return false;
    }
    std::string contents;
    RepositoryDescription actual;
    if (!read_file(version_directory / DESCRIPTION_FILENAME, contents) ||
        !parse_repository_description(contents, RepositoryPackageType::Plugin, actual, error_message))
        return false;
    return descriptions_match(actual, description, error_message);
}

} // namespace

RepositoryPackageCache::RepositoryPackageCache(boost::filesystem::path data_directory,
                                               const RepositoryPackageCacheAdapter &adapter)
    : m_data_directory(std::move(data_directory)), m_adapter(adapter)
{
}

bool RepositoryPackageCache::prepare_layout(bool &purged, std::string &error_message) const
{
    purged = false;
    const boost::filesystem::path root = type_directory();
    const boost::filesystem::path marker = root / CACHE_LAYOUT_FILENAME;
    try {
        std::string current_version;
        if (read_file(marker, current_version) && current_version == CACHE_LAYOUT_VERSION)
            return true;

        // Missing marker means callers must repopulate durable sources even
        // when the cache directory itself does not exist yet.
        purged = true;
        if (boost::filesystem::exists(root)) {
            boost::filesystem::remove_all(root);
        }
        boost::filesystem::create_directories(root);
        boost::nowide::ofstream stream(marker.string(), std::ios::out | std::ios::trunc);
        stream << CACHE_LAYOUT_VERSION;
        if (!stream.good()) {
            error_message = "Cannot write repository cache layout marker.";
            return false;
        }
        return true;
    } catch (const boost::filesystem::filesystem_error &error) {
        error_message = error.what();
        return false;
    }
}

boost::filesystem::path RepositoryPackageCache::type_directory() const
{
    return m_data_directory / "cache" / package_type_directory(m_adapter.package_type());
}

boost::filesystem::path RepositoryPackageCache::repository_directory(const std::string &id) const
{
    return type_directory() / safe_id(id);
}

boost::filesystem::path RepositoryPackageCache::repository_description_path(const std::string &id) const
{
    const std::string filesystem_id = safe_id(id);
    return type_directory() / filesystem_id / (filesystem_id + ".ini");
}

boost::filesystem::path RepositoryPackageCache::repository_tags_path(const std::string &id) const
{
    return repository_directory(id) / "tags.json";
}

boost::filesystem::path RepositoryPackageCache::repository_logs_directory(const std::string &id) const
{
    return repository_directory(id) / "logs";
}

boost::filesystem::path RepositoryPackageCache::version_directory(const std::string &id,
                                                                  const std::string &package_version,
                                                                  const std::string &slicer_version) const
{
    return repository_directory(id) / version_directory_name(package_version, slicer_version);
}

bool RepositoryPackageCache::save_repository_description(const RepositoryDescription &description,
                                                         std::string &error_message) const
{
    if (description.type != m_adapter.package_type() || description.id.empty()) {
        error_message = "Repository description has the wrong package type or no id.";
        return false;
    }

    try {
        const boost::filesystem::path root = repository_directory(description.id);
        const boost::filesystem::path descriptor = repository_description_path(description.id);
        if (!repository_root_accepts_id(descriptor, m_adapter.package_type(), description.id, error_message))
            return false;
        boost::filesystem::create_directories(root);
        RepositoryDescription root_description = description;
        root_description.package_version.clear();
        root_description.slicer_version.clear();

        const boost::filesystem::path staging = root /
            boost::filesystem::unique_path(".description-%%%%-%%%%.ini");
        const boost::filesystem::path backup = root /
            boost::filesystem::unique_path(".description-previous-%%%%-%%%%.ini");
        if (!write_description_file(staging, root_description, false, error_message)) {
            boost::system::error_code cleanup_error;
            boost::filesystem::remove(staging, cleanup_error);
            return false;
        }

        // Windows cannot rename over an existing file. Keep the previous
        // descriptor beside the staging file until the new descriptor is in
        // place, then remove it without making cleanup failure fatal.
        const bool replacing = boost::filesystem::is_regular_file(descriptor);
        try {
            if (replacing)
                boost::filesystem::rename(descriptor, backup);
            boost::filesystem::rename(staging, descriptor);
        } catch (...) {
            if (boost::filesystem::is_regular_file(backup) && !boost::filesystem::exists(descriptor))
                boost::filesystem::rename(backup, descriptor);
            boost::system::error_code cleanup_error;
            boost::filesystem::remove(staging, cleanup_error);
            throw;
        }
        if (replacing) {
            boost::system::error_code cleanup_error;
            boost::filesystem::remove(backup, cleanup_error);
            if (cleanup_error)
                BOOST_LOG_TRIVIAL(warning) << "Cannot remove previous repository description '"
                                           << backup.string() << "': " << cleanup_error.message();
        }
        return true;
    } catch (const boost::filesystem::filesystem_error &error) {
        error_message = error.what();
        return false;
    }
}

bool RepositoryPackageCache::cache_simple(const boost::filesystem::path &source,
                                          RepositoryCachedVersion &cached,
                                          std::string &error_message) const
{
    return cache_source(source, RepositoryPackageSource::Simple, std::nullopt, cached, error_message);
}

bool RepositoryPackageCache::cache_archive(const boost::filesystem::path &archive_path,
                                           const std::optional<RepositoryDescription> &expected,
                                           RepositoryCachedVersion &cached,
                                           std::string &error_message) const
{
    const boost::filesystem::path extraction = type_directory() /
        boost::filesystem::unique_path(".extract-%%%%-%%%%");
    try {
        if (!extract_repository_archive(archive_path, extraction, error_message)) {
            boost::filesystem::remove_all(extraction);
            return false;
        }

        RepositoryDescription ignored;
        std::string direct_error;
        boost::filesystem::path package_root = extraction;
        if (!m_adapter.inspect(package_root, RepositoryPackageSource::Package, expected, ignored, direct_error)) {
            boost::filesystem::directory_iterator it(extraction), end;
            if (it == end || !boost::filesystem::is_directory(it->path())) {
                boost::filesystem::remove_all(extraction);
                error_message = std::move(direct_error);
                return false;
            }
            package_root = it->path();
            ++it;
            if (it != end) {
                boost::filesystem::remove_all(extraction);
                error_message = "Package archive contains more than one root entry.";
                return false;
            }
        }

        const bool succeeded = cache_source(package_root, RepositoryPackageSource::Package,
                                            expected, cached, error_message);
        boost::filesystem::remove_all(extraction);
        return succeeded;
    } catch (const boost::filesystem::filesystem_error &error) {
        boost::system::error_code ignored_error;
        boost::filesystem::remove_all(extraction, ignored_error);
        error_message = error.what();
        return false;
    }
}

bool RepositoryPackageCache::cache_package_directory(
    const boost::filesystem::path &package_directory,
    const std::optional<RepositoryDescription> &expected,
    RepositoryCachedVersion &cached,
    std::string &error_message) const
{
    return cache_source(package_directory, RepositoryPackageSource::Package, expected, cached, error_message);
}

bool RepositoryPackageCache::cache_source(const boost::filesystem::path &source,
                                          RepositoryPackageSource source_type,
                                          const std::optional<RepositoryDescription> &expected,
                                          RepositoryCachedVersion &cached,
                                          std::string &error_message) const
{
    RepositoryDescription description;
    if (!m_adapter.inspect(source, source_type, expected, description, error_message))
        return false;
    apply_description_defaults(description);
    if (!Semver::parse(description.package_version) || !Semver::parse(description.slicer_version)) {
        error_message = "Repository package contains an invalid package or slicer version.";
        return false;
    }
    return publish(source, source_type, description, cached, error_message);
}

bool RepositoryPackageCache::publish(const boost::filesystem::path &source,
                                     RepositoryPackageSource source_type,
                                     const RepositoryDescription &description,
                                     RepositoryCachedVersion &cached,
                                     std::string &error_message) const
{
    const boost::filesystem::path root = repository_directory(description.id);
    const boost::filesystem::path destination = version_directory(
        description.id, description.package_version, description.slicer_version);
    const boost::filesystem::path staging = root /
        boost::filesystem::unique_path(".publish-%%%%-%%%%");
    const boost::filesystem::path backup = root /
        boost::filesystem::unique_path(".previous-%%%%-%%%%");
    bool backup_created = false;
    bool published = false;
    try {
        if (!repository_root_accepts_id(repository_description_path(description.id),
                                        m_adapter.package_type(), description.id, error_message))
            return false;
        boost::filesystem::create_directories(root);
        if (!m_adapter.stage(source, source_type, description, staging, error_message) ||
            !m_adapter.validate(staging, description, error_message)) {
            boost::filesystem::remove_all(staging);
            return false;
        }

        const bool replacing = boost::filesystem::exists(destination);
        if (replacing) {
            boost::filesystem::rename(destination, backup);
            backup_created = true;
        }
        try {
            boost::filesystem::rename(staging, destination);
            published = true;
        } catch (...) {
            if (replacing && !boost::filesystem::exists(destination))
                boost::filesystem::rename(backup, destination);
            throw;
        }
        if (!refresh_repository_description(description.id, description, error_message)) {
            // The root descriptor is part of publication. Restore the previous
            // exact version when it existed, or remove the newly added version,
            // so callers never observe a half-published package.
            boost::filesystem::remove_all(destination);
            if (replacing)
                boost::filesystem::rename(backup, destination);
            return false;
        }
        if (replacing) {
            boost::system::error_code cleanup_error;
            boost::filesystem::remove_all(backup, cleanup_error);
            if (cleanup_error)
                BOOST_LOG_TRIVIAL(warning) << "Cannot remove previous cached version '"
                                           << backup.string() << "': " << cleanup_error.message();
        }
        cached.description = description;
        cached.directory = destination;
        return true;
    } catch (const boost::filesystem::filesystem_error &error) {
        boost::system::error_code ignored_error;
        if (published)
            boost::filesystem::remove_all(destination, ignored_error);
        if (backup_created && !boost::filesystem::exists(destination)) {
            try {
                boost::filesystem::rename(backup, destination);
            } catch (const boost::filesystem::filesystem_error &restore_error) {
                BOOST_LOG_TRIVIAL(error) << "Cannot restore cached repository version '"
                                         << destination.string() << "': " << restore_error.what();
            }
        }
        boost::filesystem::remove_all(staging, ignored_error);
        error_message = error.what();
        return false;
    }
}

bool RepositoryPackageCache::refresh_repository_description(
    const std::string &id,
    const RepositoryDescription &fallback,
    std::string &error_message) const
{
    RepositoryDescription selected = fallback;
    RepositoryDescription existing;
    std::string contents;
    const boost::filesystem::path descriptor = repository_description_path(id);
    if (read_file(descriptor, contents)) {
        std::string parse_error;
        if (parse_repository_description(contents, m_adapter.package_type(), existing, parse_error) &&
            !existing.config_update_rest.empty() && selected.config_update_rest.empty())
            selected.config_update_rest = existing.config_update_rest;
    }

    const boost::filesystem::path root = repository_directory(id);
    for (boost::filesystem::directory_iterator it(root), end; it != end; ++it) {
        if (!boost::filesystem::is_directory(it->path()))
            continue;
        std::string package_version;
        std::string slicer_version;
        if (!parse_version_directory_name(it->path().filename().string(), package_version, slicer_version))
            continue;
        std::string version_contents;
        RepositoryDescription candidate;
        std::string parse_error;
        if (!read_file(it->path() / DESCRIPTION_FILENAME, version_contents) ||
            !parse_repository_description(version_contents, m_adapter.package_type(), candidate, parse_error))
            continue;
        if (newer_description(candidate, selected)) {
            const std::string preserved_url = selected.config_update_rest;
            selected = std::move(candidate);
            if (selected.config_update_rest.empty())
                selected.config_update_rest = preserved_url;
        }
    }
    return save_repository_description(selected, error_message);
}

std::vector<RepositoryCachedEntry> RepositoryPackageCache::scan() const
{
    std::vector<RepositoryCachedEntry> entries;
    if (!boost::filesystem::is_directory(type_directory()))
        return entries;

    try {
        for (boost::filesystem::directory_iterator root_it(type_directory()), root_end;
             root_it != root_end; ++root_it) {
            if (!boost::filesystem::is_directory(root_it->path()) ||
                root_it->path().filename().string().front() == '.')
                continue;

            const std::string filesystem_id = root_it->path().filename().string();
            std::string contents;
            RepositoryDescription root_description;
            std::string error_message;
            if (!read_file(root_it->path() / (filesystem_id + ".ini"), contents) ||
                !parse_repository_description(contents, m_adapter.package_type(), root_description, error_message) ||
                safe_id(root_description.id) != filesystem_id) {
                if (error_message.empty())
                    error_message = "Repository descriptor id does not match its sanitized directory name.";
                BOOST_LOG_TRIVIAL(warning) << "Ignoring repository cache '" << root_it->path().string()
                                           << "': " << error_message;
                continue;
            }

            RepositoryCachedEntry entry;
            entry.description = root_description;
            entry.directory = root_it->path();
            for (boost::filesystem::directory_iterator version_it(root_it->path()), version_end;
                 version_it != version_end; ++version_it) {
                if (!boost::filesystem::is_directory(version_it->path()))
                    continue;
                std::string package_version;
                std::string slicer_version;
                if (!parse_version_directory_name(version_it->path().filename().string(),
                                                  package_version, slicer_version))
                    continue;
                RepositoryDescription version_description;
                std::string version_contents;
                if (!read_file(version_it->path() / DESCRIPTION_FILENAME, version_contents) ||
                    !parse_repository_description(version_contents, m_adapter.package_type(),
                                                  version_description, error_message) ||
                    version_description.id != root_description.id ||
                    version_description.package_version != package_version ||
                    version_description.slicer_version != slicer_version ||
                    !m_adapter.validate(version_it->path(), version_description, error_message)) {
                    BOOST_LOG_TRIVIAL(warning) << "Ignoring repository package '"
                                               << version_it->path().string() << "': " << error_message;
                    continue;
                }
                entry.versions.push_back({std::move(version_description), version_it->path()});
            }
            std::sort(entry.versions.begin(), entry.versions.end(),
                      [](const RepositoryCachedVersion &left, const RepositoryCachedVersion &right) {
                          return newer_description(left.description, right.description);
                      });
            entries.emplace_back(std::move(entry));
        }
    } catch (const boost::filesystem::filesystem_error &error) {
        BOOST_LOG_TRIVIAL(warning) << "Cannot finish scanning repository cache '"
                                   << type_directory().string() << "': " << error.what();
    }
    return entries;
}

std::string RepositoryPackageCache::safe_id(const std::string &id)
{
    std::string safe;
    safe.reserve(id.size());
    for (const unsigned char character : id)
        safe.push_back(character < 0x80 && (std::isalnum(character) || character == '.' ||
                                           character == '_' || character == '-') ?
                       static_cast<char>(character) : '-');
    while (safe.find("..") != std::string::npos)
        safe.replace(safe.find(".."), 2, "--");
    if (safe.empty() || safe == "." || safe == "..")
        safe = "repository";

    // Leading/trailing dots and DOS device names are not portable directory
    // names. Prefixing keeps sanitization deterministic without changing the
    // raw id stored in repository descriptors.
    if (safe.front() == '.' || safe.back() == '.')
        safe.insert(safe.begin(), '_');
    std::string uppercase = safe;
    std::transform(uppercase.begin(), uppercase.end(), uppercase.begin(),
                   [](unsigned char character) { return static_cast<char>(std::toupper(character)); });
    const bool reserved_device = uppercase == "CON" || uppercase == "PRN" || uppercase == "AUX" ||
                                 uppercase == "NUL" ||
                                 (uppercase.size() == 4 &&
                                  (uppercase.compare(0, 3, "COM") == 0 || uppercase.compare(0, 3, "LPT") == 0) &&
                                  uppercase[3] >= '1' && uppercase[3] <= '9');
    if (reserved_device)
        safe.insert(safe.begin(), '_');
    return safe;
}

std::string RepositoryPackageCache::version_directory_name(const std::string &package_version,
                                                           const std::string &slicer_version)
{
    return package_version + '=' + slicer_version;
}

const RepositoryPackageCacheAdapter &vendor_repository_cache_adapter()
{
    static const VendorRepositoryPackageCacheAdapter adapter;
    return adapter;
}

const RepositoryPackageCacheAdapter &plugin_repository_cache_adapter()
{
    static const PluginRepositoryPackageCacheAdapter adapter;
    return adapter;
}

} // namespace Slic3r
