///|/ Copyright (c) SuperSlicer 2026 Durand Remi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/

/*
Plugin repository and package transactions
===========================================

This file implements the durable on-disk protocol used to cache, validate,
install, and remove plugin packages. A package is never copied directly into
the live plugin directory from an unvalidated archive or cache entry.

The normal execution flows are:

    cache_plugin_package_archive()
    |-- prepare the cache layout
    |-- extract the archive into a temporary package directory
    `-- validate its description, versions, and native/Python entry point

    reconcile_installed_plugin_packages()
    |-- build the complete desired package transaction
    |-- stage every replacement beside the live package directory
    |-- validate each staged copy independently
    `-- commit all replacements and removals as one transaction

    request_plugin_install() / request_plugin_uninstall()
    |-- validate the requested package identity
    |-- update the durable activation configuration
    `-- let the next startup perform the package transaction

Validation checks package names and semantic versions, verifies that
`description.ini` identifies the expected package type and ID, checks
`version.ini`, and requires either the platform plugin library or one
unambiguous Python entry point. The live directory is modified only after all
desired replacements have been copied and validated. A failure restores the
previous live repository, while cleanup failures are reported separately so a
successful installation is not confused with a cleanup warning.

The cache is also the boundary between downloaded content and executable
content. Startup may republish embedded bundles into it; runtime cache
operations do not unexpectedly republish those resources. Package directories
not present in the desired installed set are removed from the live repository,
but the activation file is managed by the separate activation-configuration
module.
*/

#include "PluginRepository.hpp"

#include <algorithm>
#include <exception>
#include <iterator>
#include <map>
#include <sstream>
#include <string>
#include <utility>

#include <boost/algorithm/string/predicate.hpp>
#include <boost/algorithm/string/trim.hpp>
#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>
#include <boost/nowide/fstream.hpp>
#include <boost/property_tree/ini_parser.hpp>
#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ptree.hpp>

#include "libslic3r/Exception.hpp"
#include "libslic3r/FilesystemTransaction.hpp"
#include "libslic3r/Semver.hpp"
#include "libslic3r/Updater/RepositoryPackageCache.hpp"
#include "libslic3r/miniz_extension.hpp"
#include "libslic3r/Utils.hpp"

namespace Slic3r {
namespace {

const char *const PLUGIN_DIRECTORY = "plugins";
const char *const DESCRIPTION_FILENAME = "description.ini";
const char *const VERSION_FILENAME = "version.ini";

enum class PluginPackageTransactionAction {
    Replace,
    Remove
};

// One entry retains the package context needed before handing its paths to the
// content-agnostic filesystem transaction.
struct PluginPackageTransactionEntry {
    PluginPackageTransactionAction action = PluginPackageTransactionAction::Replace;
    std::string package_name;
    PluginInstalledVersion version;
    boost::filesystem::path source;
    boost::filesystem::path destination;
    boost::filesystem::path staging;
};

#ifdef SLIC3R_PLUGIN_REPOSITORY_TESTING
PluginPackageTransactionTestHook g_plugin_package_transaction_test_hook;
#endif

const char *plugin_package_library_filename();
bool parse_repository_enabled_value(const std::string &value, bool &enabled);
bool is_valid_repository_id(const std::string &repository_id);
bool parse_bundle_filename(const boost::filesystem::path &archive_path,
                           std::string &package_name,
                           PluginInstalledVersion &version);
bool is_safe_archive_entry(const std::string &entry_name);
bool copy_directory_tree(const boost::filesystem::path &source,
                         const boost::filesystem::path &destination,
                         std::string &error_message);
boost::filesystem::path find_plugin_python_entry(const boost::filesystem::path &package_root,
                                                 const std::string &package_name);
bool read_plugin_version(const boost::filesystem::path &package_root,
                         PluginInstalledVersion &version,
                         std::string &error_message);
bool validate_plugin_package(const boost::filesystem::path &package_root,
                             const std::string &package_name,
                             const PluginInstalledVersion &version,
                             std::string &error_message);
bool installed_package_matches(const boost::filesystem::path &package_root,
                               const std::string &package_name,
                               const PluginInstalledVersion &version);
// Build a deterministic transaction after validating every desired cache
// entry. The function does not modify any live package.
bool build_plugin_package_transaction(const boost::filesystem::path &data_directory,
                                      const PluginActivationConfig &config,
                                      std::vector<PluginPackageTransactionEntry> &entries,
                                      std::string &error_message);
// Copy every replacement beside the live packages and validate the copies.
// Failure here leaves the complete live repository untouched.
bool stage_plugin_package_transaction(std::vector<PluginPackageTransactionEntry> &entries,
                                      std::string &error_message);
// Log disposable artifacts which could not be removed after a stable commit or
// rollback without turning those warnings into a package failure.
void log_plugin_transaction_cleanup_warnings(const FilesystemTransactionResult &result);
// Remove hidden transaction directories left by an interrupted older process
// only after the desired live set has been committed successfully.
void cleanup_stale_plugin_transaction_directories(const boost::filesystem::path &plugin_directory);
// Best-effort removal is used only for hidden transaction artifacts; failures
// are logged because the loader never treats those paths as packages.
void remove_plugin_transaction_path(const boost::filesystem::path &path, const char *purpose);
#ifdef SLIC3R_PLUGIN_REPOSITORY_TESTING
// Invoke the package-specific seam before staging copies. Commit and rollback
// failures use the generic FilesystemTransaction test seam.
void invoke_plugin_package_transaction_test_hook(PluginPackageTransactionTestPoint point,
                                                 const std::string &package_name);
#endif

#ifdef SLIC3R_PLUGIN_REPOSITORY_TESTING
void invoke_plugin_package_transaction_test_hook(PluginPackageTransactionTestPoint point,
                                                 const std::string &package_name)
{
    if (g_plugin_package_transaction_test_hook)
        g_plugin_package_transaction_test_hook(point, package_name);
}
#endif

const char *plugin_package_library_filename()
{
#ifdef _WIN32
    return "plugin.dll";
#elif defined(__APPLE__)
    return "plugin.dylib";
#else
    return "plugin.so";
#endif
}

bool parse_repository_enabled_value(const std::string &value, bool &enabled)
{
    if (boost::algorithm::iequals(value, "1") || boost::algorithm::iequals(value, "true") ||
        boost::algorithm::iequals(value, "yes") || boost::algorithm::iequals(value, "on") ||
        boost::algorithm::iequals(value, "enabled")) {
        enabled = true;
        return true;
    }
    if (boost::algorithm::iequals(value, "0") || boost::algorithm::iequals(value, "false") ||
        boost::algorithm::iequals(value, "no") || boost::algorithm::iequals(value, "off") ||
        boost::algorithm::iequals(value, "disabled")) {
        enabled = false;
        return true;
    }
    return false;
}

bool is_valid_repository_id(const std::string &repository_id)
{
    if (repository_id.empty() || repository_id == "." || repository_id == ".." ||
        repository_id.back() == '.' || repository_id.back() == ' ')
        return false;
    for (const unsigned char character : repository_id)
        if (character < 0x20 || character == '/' || character == '\\' || character == ':' ||
            character == '*' || character == '?' || character == '"' || character == '<' ||
            character == '>' || character == '|')
            return false;
    return true;
}

bool parse_bundle_filename(const boost::filesystem::path &archive_path,
                           std::string &package_name,
                           PluginInstalledVersion &version)
{
    if (!boost::algorithm::iequals(archive_path.extension().string(), ".zip"))
        return false;

    const std::string stem = archive_path.stem().string();
    const size_t slicer_separator = stem.rfind('_');
    if (slicer_separator == std::string::npos || slicer_separator == 0 || slicer_separator + 1 == stem.size())
        return false;
    const size_t package_separator = stem.rfind('_', slicer_separator - 1);
    if (package_separator == std::string::npos || package_separator == 0 || package_separator + 1 == slicer_separator)
        return false;

    package_name = stem.substr(0, package_separator);
    version.package_version = stem.substr(package_separator + 1, slicer_separator - package_separator - 1);
    version.slicer_version = stem.substr(slicer_separator + 1);
    return is_valid_plugin_package_name(package_name) &&
           is_valid_plugin_package_version(version.package_version) &&
           is_valid_plugin_package_version(version.slicer_version);
}

bool is_safe_archive_entry(const std::string &entry_name)
{
    if (entry_name.empty() || entry_name.front() == '/' || entry_name.front() == '\\' || entry_name.find(':') != std::string::npos)
        return false;

    std::string normalized_name = entry_name;
    for (char &character : normalized_name)
        if (character == '\\')
            character = '/';
    while (!normalized_name.empty() && normalized_name.back() == '/')
        normalized_name.pop_back();
    if (normalized_name.empty())
        return false;

    std::istringstream components(normalized_name);
    std::string component;
    while (std::getline(components, component, '/'))
        if (component.empty() || component == "." || component == "..")
            return false;
    return true;
}

bool copy_directory_tree(const boost::filesystem::path &source,
                         const boost::filesystem::path &destination,
                         std::string &error_message)
{
    try {
        boost::filesystem::create_directories(destination);
        for (boost::filesystem::recursive_directory_iterator it(source), end; it != end; ++it) {
            const boost::filesystem::path relative_path = boost::filesystem::relative(it->path(), source);
            const boost::filesystem::path output_path = destination / relative_path;
            if (boost::filesystem::is_directory(it->path()))
                boost::filesystem::create_directories(output_path);
            else if (boost::filesystem::is_regular_file(it->path())) {
                boost::filesystem::create_directories(output_path.parent_path());
                boost::filesystem::copy_file(it->path(), output_path, boost::filesystem::copy_options::overwrite_existing);
            } else {
                error_message = "Package '" + source.string() + "' contains an unsupported filesystem entry.";
                return false;
            }
        }
    } catch (const boost::filesystem::filesystem_error &error) {
        error_message = error.what();
        return false;
    }
    return true;
}

// Python packages use one predictable root entry so both validation and the
// runtime loader agree on exactly which script owns plugin registration.
boost::filesystem::path find_plugin_python_entry(const boost::filesystem::path &package_root,
                                                 const std::string &package_name)
{
    const boost::filesystem::path named_entry = package_root / (package_name + ".py");
    if (boost::filesystem::is_regular_file(named_entry))
        return named_entry;
    const boost::filesystem::path conventional_entry = package_root / "plugin.py";
    if (boost::filesystem::is_regular_file(conventional_entry))
        return conventional_entry;

    boost::filesystem::path unique_entry;
    for (boost::filesystem::directory_iterator it(package_root), end; it != end; ++it) {
        if (!boost::filesystem::is_regular_file(it->path()) || it->path().extension() != ".py")
            continue;
        if (!unique_entry.empty())
            return {};
        unique_entry = it->path();
    }
    return unique_entry;
}

bool read_plugin_version(const boost::filesystem::path &package_root,
                         PluginInstalledVersion &version,
                         std::string &error_message)
{
    const boost::filesystem::path version_path = package_root / VERSION_FILENAME;
    try {
        boost::property_tree::ptree tree;
        boost::property_tree::read_ini(version_path.string(), tree);
        const boost::property_tree::ptree &plugin = tree.get_child("plugin");
        version.package_version = plugin.get<std::string>("package_version", std::string());
        version.slicer_version = plugin.get<std::string>("slicer_version", std::string());
        if (!is_valid_plugin_package_version(version.package_version) ||
            !is_valid_plugin_package_version(version.slicer_version)) {
            error_message = "Plugin version.ini contains an invalid package or slicer version.";
            return false;
        }
        return true;
    } catch (const std::exception &error) {
        error_message = "Cannot read plugin version.ini: " + std::string(error.what());
        return false;
    }
}

bool validate_plugin_package(const boost::filesystem::path &package_root,
                             const std::string &package_name,
                             const PluginInstalledVersion &version,
                             std::string &error_message)
{
    if (!is_valid_plugin_package_name(package_name) ||
        !is_valid_plugin_package_version(version.package_version) ||
        !is_valid_plugin_package_version(version.slicer_version)) {
        error_message = "Invalid plugin package name or version.";
        return false;
    }

    const boost::filesystem::path description_path = package_root / DESCRIPTION_FILENAME;
    boost::nowide::ifstream stream(description_path.string());
    if (!stream) {
        error_message = "Plugin package '" + package_name + "' does not contain description.ini.";
        return false;
    }
    const std::string contents((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
    RepositoryDescription description;
    if (!parse_repository_description(contents, RepositoryPackageType::Plugin, description, error_message))
        return false;
    if (description.id != package_name) {
        error_message = "Plugin description.ini does not match package '" + package_name + "'.";
        return false;
    }

    PluginInstalledVersion stored_version;
    if (!read_plugin_version(package_root, stored_version, error_message) ||
        stored_version.package_version != version.package_version ||
        stored_version.slicer_version != version.slicer_version) {
        if (error_message.empty())
            error_message = "Plugin version.ini does not match the requested cached version.";
        return false;
    }

    const boost::filesystem::path library_path = package_root / plugin_package_library_filename();
    if (!boost::filesystem::is_regular_file(library_path) &&
        find_plugin_python_entry(package_root, package_name).empty()) {
        error_message = "Plugin package '" + package_name + "' contains neither '" +
                        plugin_package_library_filename() + "' nor an unambiguous Python entry point.";
        return false;
    }
    return true;
}

bool installed_package_matches(const boost::filesystem::path &package_root,
                               const std::string &package_name,
                               const PluginInstalledVersion &version)
{
    std::string ignored_error;
    return boost::filesystem::is_directory(package_root) &&
           validate_plugin_package(package_root, package_name, version, ignored_error);
}

bool build_plugin_package_transaction(const boost::filesystem::path &data_directory,
                                      const PluginActivationConfig &config,
                                      std::vector<PluginPackageTransactionEntry> &entries,
                                      std::string &error_message)
{
    entries.clear();

    // Validate the complete desired set before even creating the live plugin
    // directory. Cache problems therefore cannot start a filesystem commit.
    for (const auto &[package_name, version] : config.installed)
        if (!plugin_package_cache_is_valid(data_directory, package_name, version, error_message))
            return false;

    const boost::filesystem::path plugin_directory = data_directory / PLUGIN_DIRECTORY;
    try {
        boost::filesystem::create_directories(plugin_directory);

        // Only packages whose complete live payload differs need staging. A
        // matching directory may contain local markers which must be retained.
        for (const auto &[package_name, version] : config.installed) {
            const boost::filesystem::path destination = plugin_directory / package_name;
            if (installed_package_matches(destination, package_name, version))
                continue;

            PluginPackageTransactionEntry entry;
            entry.action = PluginPackageTransactionAction::Replace;
            entry.package_name = package_name;
            entry.version = version;
            entry.source = repository_package_cache_path(
                data_directory, RepositoryPackageType::Plugin, package_name,
                version.package_version, version.slicer_version);
            entry.destination = destination;
            entry.staging = plugin_directory /
                boost::filesystem::unique_path("." + package_name + ".install-%%%%-%%%%");
            entries.emplace_back(std::move(entry));
        }

        // Enumerate removals before committing. Sorting them makes error and
        // rollback order deterministic on every supported filesystem.
        std::vector<boost::filesystem::path> removals;
        for (boost::filesystem::directory_iterator it(plugin_directory), end; it != end; ++it) {
            if (!boost::filesystem::is_directory(it->path()))
                continue;
            const std::string package_name = it->path().filename().string();
            if (package_name.empty() || package_name.front() == '.' || config.installed.count(package_name) != 0)
                continue;
            removals.emplace_back(it->path());
        }
        std::sort(removals.begin(), removals.end());
        for (const boost::filesystem::path &destination : removals) {
            PluginPackageTransactionEntry entry;
            entry.action = PluginPackageTransactionAction::Remove;
            entry.package_name = destination.filename().string();
            entry.destination = destination;
            entries.emplace_back(std::move(entry));
        }
    } catch (const boost::filesystem::filesystem_error &error) {
        error_message = "Cannot prepare plugin package transaction: " + std::string(error.what());
        return false;
    }
    return true;
}

bool stage_plugin_package_transaction(std::vector<PluginPackageTransactionEntry> &entries,
                                      std::string &error_message)
{
    for (PluginPackageTransactionEntry &entry : entries) {
        if (entry.action != PluginPackageTransactionAction::Replace)
            continue;

        // Copy every replacement before touching live directories. Validating
        // the copy catches incomplete reads independently from cache validity.
        try {
#ifdef SLIC3R_PLUGIN_REPOSITORY_TESTING
            invoke_plugin_package_transaction_test_hook(
                PluginPackageTransactionTestPoint::BeforeStageCopy, entry.package_name);
#endif
            if (copy_directory_tree(entry.source, entry.staging, error_message) &&
                validate_plugin_package(entry.staging, entry.package_name, entry.version, error_message))
                continue;
        } catch (const std::exception &error) {
            error_message = error.what();
        } catch (...) {
            error_message = "unknown staging failure";
        }

        error_message = "Cannot stage plugin package '" + entry.package_name + "': " + error_message;
        return false;
    }
    return true;
}

void log_plugin_transaction_cleanup_warnings(const FilesystemTransactionResult &result)
{
    for (const FilesystemTransactionFailure &warning : result.cleanup_warnings)
        BOOST_LOG_TRIVIAL(warning) << "Plugin package transaction cleanup warning: "
                                   << format_filesystem_transaction_failure(warning);
}

void cleanup_stale_plugin_transaction_directories(const boost::filesystem::path &plugin_directory)
{
    try {
        if (!boost::filesystem::is_directory(plugin_directory))
            return;
        for (boost::filesystem::directory_iterator it(plugin_directory), end; it != end; ++it) {
            if (!boost::filesystem::is_directory(it->path()))
                continue;
            const std::string filename = it->path().filename().string();
            if (filename.empty() || filename.front() != '.' ||
                (filename.find(".install-") == std::string::npos &&
                 filename.find(".backup-") == std::string::npos &&
                 filename.find(".transaction-backup-") == std::string::npos))
                continue;
            remove_plugin_transaction_path(it->path(), "stale transaction");
        }
    } catch (const boost::filesystem::filesystem_error &error) {
        BOOST_LOG_TRIVIAL(warning) << "Cannot enumerate stale plugin transaction directories: " << error.what();
    }
}

void remove_plugin_transaction_path(const boost::filesystem::path &path, const char *purpose)
{
    if (path.empty())
        return;
    boost::system::error_code error;
    boost::filesystem::remove_all(path, error);
    if (error)
        BOOST_LOG_TRIVIAL(warning) << "Cannot remove plugin transaction " << purpose << " path '"
                                   << path.string() << "': " << error.message();
}

} // namespace

#ifdef SLIC3R_PLUGIN_REPOSITORY_TESTING
void set_plugin_package_transaction_test_hook(PluginPackageTransactionTestHook hook)
{
    g_plugin_package_transaction_test_hook = std::move(hook);
}
#endif

bool parse_repository_description(const std::string &contents,
                                  RepositoryPackageType expected_type,
                                  RepositoryDescription &description,
                                  std::string &error_message)
{
    try {
        boost::property_tree::ptree tree;
        std::istringstream stream(contents);
        boost::property_tree::read_ini(stream, tree);
        const char *section_name = expected_type == RepositoryPackageType::Vendor ? "vendor" : "plugin";
        const char *other_section_name = expected_type == RepositoryPackageType::Vendor ? "plugin" : "vendor";
        if (tree.get_child_optional(other_section_name)) {
            error_message = "Repository description.ini contains both [vendor] and [plugin] sections.";
            return false;
        }
        const boost::property_tree::ptree &section = tree.get_child(section_name);
        description = {};
        description.type = expected_type;
        description.id = boost::algorithm::trim_copy(section.get<std::string>("id"));
        description.name = section.get<std::string>("name", description.id);
        description.full_name = section.get<std::string>("full_name", description.name);
        description.description = section.get<std::string>("description", std::string());
        description.config_update_rest = section.get<std::string>("config_update_rest", std::string());
        description.slicer = section.get<std::string>("slicer", std::string());
        bool is_internal = false;
        parse_repository_enabled_value(section.get<std::string>("internal", "0"), is_internal);
        description.is_internal = is_internal;
        // Version-looking keys are accepted for compatibility with packages
        // produced before versions were separated, but are intentionally not
        // copied into the generic repository description.
        if (!is_valid_repository_id(description.id)) {
            error_message = "Repository description contains an invalid id '" + description.id + "'.";
            return false;
        }
    } catch (const std::exception &error) {
        error_message = "Cannot parse repository description.ini: " + std::string(error.what());
        return false;
    }
    return true;
}

bool parse_repository_versions(const std::string &json,
                               std::vector<RepositoryPackageVersion> &versions,
                               std::string &error_message)
{
    try {
        boost::property_tree::ptree root;
        std::istringstream stream(json);
        boost::property_tree::read_json(stream, root);
        std::map<std::string, size_t> known_tags;
        for (size_t index = 0; index < versions.size(); ++index)
            known_tags.emplace(versions[index].tag, index);

        for (const boost::property_tree::ptree::value_type &entry : root) {
            const std::string tag = entry.second.get<std::string>("name", std::string());
            const size_t separator = tag.find('=');
            if (separator == std::string::npos || separator == 0 || separator + 1 == tag.size() ||
                tag.find('=', separator + 1) != std::string::npos)
                continue;
            const std::string package_version = tag.substr(0, separator);
            const std::string slicer_version = tag.substr(separator + 1);
            if (!Semver::parse(package_version) || !Semver::parse(slicer_version))
                continue;

            RepositoryPackageVersion parsed;
            parsed.package_version = package_version;
            parsed.slicer_version = slicer_version;
            parsed.url_zip = entry.second.get<std::string>("zipball_url", std::string());
            parsed.commit_sha = entry.second.get<std::string>("commit.sha", std::string());
            parsed.commit_url = entry.second.get<std::string>("commit.url", std::string());
            parsed.tag = tag;
            const std::map<std::string, size_t>::const_iterator known = known_tags.find(tag);
            if (known == known_tags.end()) {
                known_tags.emplace(tag, versions.size());
                versions.emplace_back(std::move(parsed));
            } else {
                versions[known->second] = std::move(parsed);
            }
        }
    } catch (const std::exception &error) {
        error_message = "Cannot parse repository tags: " + std::string(error.what());
        return false;
    }
    return true;
}

boost::filesystem::path repository_package_cache_path(const boost::filesystem::path &data_directory,
                                                      RepositoryPackageType type,
                                                      const std::string &package_name,
                                                      const std::string &package_version,
                                                      const std::string &slicer_version)
{
    const RepositoryPackageCache cache(data_directory,
        type == RepositoryPackageType::Vendor ? vendor_repository_cache_adapter() :
                                                plugin_repository_cache_adapter());
    return cache.version_directory(package_name, package_version, slicer_version);
}

boost::filesystem::path repository_cache_root_path(const boost::filesystem::path &data_directory,
                                                   RepositoryPackageType type,
                                                   const std::string &package_name)
{
    const RepositoryPackageCache cache(data_directory,
        type == RepositoryPackageType::Vendor ? vendor_repository_cache_adapter() :
                                                plugin_repository_cache_adapter());
    return cache.repository_directory(package_name);
}

bool extract_repository_archive(const boost::filesystem::path &archive_path,
                                const boost::filesystem::path &destination,
                                std::string &error_message)
{
    ZipReader archive(archive_path.string());
    if (!archive.success()) {
        error_message = "Cannot open package archive '" + archive_path.string() + "'.";
        return false;
    }

    const mz_uint entry_count = mz_zip_reader_get_num_files(&archive.archive);
    for (mz_uint index = 0; index < entry_count; ++index) {
        mz_zip_archive_file_stat entry = {};
        if (!mz_zip_reader_file_stat(&archive.archive, index, &entry) || !is_safe_archive_entry(entry.m_filename)) {
            error_message = "Package archive '" + archive_path.string() + "' contains an unsafe entry.";
            return false;
        }
        const std::string entry_name(entry.m_filename);
        const boost::filesystem::path output_path = destination / boost::filesystem::path(entry_name);
        if (entry.m_is_directory) {
            boost::filesystem::create_directories(output_path);
            continue;
        }
        boost::filesystem::create_directories(output_path.parent_path());
        if (!mz_zip_reader_extract_to_file(&archive.archive, entry.m_file_index, output_path.string().c_str(), 0)) {
            error_message = "Cannot extract '" + entry_name + "' from package archive '" + archive_path.string() + "'.";
            return false;
        }
    }
    return true;
}

bool cache_plugin_package_archive(const boost::filesystem::path &data_directory,
                                  const boost::filesystem::path &archive_path,
                                  const std::string &package_name,
                                  const std::string &package_version,
                                  const std::string &slicer_version,
                                  std::string &error_message)
{
    RepositoryPackageCache cache(data_directory, plugin_repository_cache_adapter());
    bool purged = false;
    if (!cache.prepare_layout(purged, error_message))
        return false;

    RepositoryPackageExpectation expected;
    expected.type = RepositoryPackageType::Plugin;
    expected.id = package_name;
    expected.version.package_version = package_version;
    expected.version.slicer_version = slicer_version;
    RepositoryCachedVersion cached;
    if (!cache.cache_archive(archive_path, expected, cached, error_message))
        return false;

    BOOST_LOG_TRIVIAL(info) << "Cached plugin package '" << package_name << "' version '" << package_version
                            << "' for slicer '" << slicer_version << "'.";
    return true;
}

bool plugin_package_cache_is_valid(const boost::filesystem::path &data_directory,
                                   const std::string &package_name,
                                   const PluginInstalledVersion &version,
                                   std::string &error_message)
{
    const boost::filesystem::path package_root = repository_package_cache_path(
        data_directory, RepositoryPackageType::Plugin, package_name, version.package_version, version.slicer_version);
    if (!boost::filesystem::is_directory(package_root)) {
        error_message = "Plugin package '" + package_name + "' version '" + version.package_version +
                        "' is not cached.";
        return false;
    }
    return validate_plugin_package(package_root, package_name, version, error_message);
}

static bool prepare_plugin_cache_impl(const boost::filesystem::path *resources_directory,
                                      const boost::filesystem::path &data_directory,
                                      const PluginActivationConfig *activation_config,
                                      std::string &error_message)
{
    RepositoryPackageCache cache(data_directory, plugin_repository_cache_adapter());
    bool purged = false;
    if (!cache.prepare_layout(purged, error_message))
        return false;

    try {
        // Only application startup republishes embedded bundles. Runtime cache
        // operations deliberately omit this source so Clear cache remains
        // effective for the rest of the current process.
        if (resources_directory != nullptr) {
            const boost::filesystem::path bundles_directory = *resources_directory / PLUGIN_DIRECTORY;
            if (boost::filesystem::is_directory(bundles_directory)) {
                for (boost::filesystem::directory_iterator it(bundles_directory), end; it != end; ++it) {
                    if (!boost::filesystem::is_regular_file(it->path()))
                        continue;
                    std::string package_name;
                    PluginInstalledVersion version;
                    if (!parse_bundle_filename(it->path(), package_name, version)) {
                        if (boost::algorithm::iequals(it->path().extension().string(), ".zip"))
                            BOOST_LOG_TRIVIAL(warning) << "Ignoring invalid plugin archive name '"
                                                       << it->path().filename().string() << "'.";
                        continue;
                    }

                    if (!cache_plugin_package_archive(data_directory, it->path(), package_name,
                                                       version.package_version, version.slicer_version,
                                                       error_message))
                        return false;
                }
            }
        }

        if (!purged)
            return true;

        PluginActivationConfig loaded_config;
        const PluginActivationConfig *config = activation_config;
        const boost::filesystem::path activation_path = plugin_activation_config_path(data_directory);
        const bool has_activation_config = boost::filesystem::is_regular_file(activation_path);
        if (config == nullptr) {
            if (has_activation_config &&
                !read_plugin_activation_config(activation_path, loaded_config, error_message))
                return false;
            config = &loaded_config;
        }

        // Only packages selected in [installed] are durable live sources. Any
        // other live directory is outside the desired package set and will be
        // removed by startup reconciliation instead of being imported again.
        const boost::filesystem::path live_plugins = data_directory / PLUGIN_DIRECTORY;
        if (boost::filesystem::is_directory(live_plugins))
            for (const auto &[package_name, version] : config->installed) {
                const boost::filesystem::path live_package = live_plugins / package_name;
                if (!boost::filesystem::is_directory(live_package))
                    continue;
                RepositoryPackageExpectation expected;
                expected.type = RepositoryPackageType::Plugin;
                expected.id = package_name;
                expected.version.package_version = version.package_version;
                expected.version.slicer_version = version.slicer_version;
                RepositoryCachedVersion cached;
                std::string cache_error;
                if (!cache.cache_package_directory(live_package, expected, cached, cache_error))
                    BOOST_LOG_TRIVIAL(warning) << "Cannot restore live plugin package '" << package_name
                                               << "': " << cache_error;
            }
    } catch (const boost::filesystem::filesystem_error &error) {
        error_message = "Cannot prepare plugin bundle cache: " + std::string(error.what());
        return false;
    }
    return true;
}

bool prepare_plugin_cache(const boost::filesystem::path &data_directory,
                          std::string &error_message)
{
    return prepare_plugin_cache_impl(nullptr, data_directory, nullptr, error_message);
}

bool prepare_plugin_bundle_cache(const boost::filesystem::path &resources_directory,
                                 const boost::filesystem::path &data_directory,
                                 std::string &error_message)
{
    return prepare_plugin_cache_impl(&resources_directory, data_directory, nullptr, error_message);
}

bool prepare_plugin_bundle_cache(const boost::filesystem::path &resources_directory,
                                 const boost::filesystem::path &data_directory,
                                 const PluginActivationConfig &activation_config,
                                 std::string &error_message)
{
    return prepare_plugin_cache_impl(&resources_directory, data_directory, &activation_config, error_message);
}

bool reconcile_installed_plugin_packages(const boost::filesystem::path &data_directory,
                                         const PluginActivationConfig &config,
                                         std::string &error_message)
{
    error_message.clear();
    std::vector<PluginPackageTransactionEntry> entries;

    // Register all paths before staging so the transaction owns every partial
    // copy if preparation returns early or throws.
    if (!build_plugin_package_transaction(data_directory, config, entries, error_message))
        return false;
    FilesystemTransaction transaction;
    for (const PluginPackageTransactionEntry &entry : entries) {
        if (entry.action == PluginPackageTransactionAction::Replace)
            transaction.add_replacement(entry.staging, entry.destination);
        else
            transaction.add_removal(entry.destination);
    }
    if (!stage_plugin_package_transaction(entries, error_message))
        return false;

    // A single content-neutral commit preserves the complete previous package
    // set until every desired replacement and removal succeeds.
    const FilesystemTransactionResult result = transaction.commit();
    log_plugin_transaction_cleanup_warnings(result);
    if (result.status == FilesystemTransactionStatus::RollbackFailed)
        throw RuntimeError("Plugin package transaction could not restore the live repository: " +
                           format_filesystem_transaction_error(result));
    if (result.status != FilesystemTransactionStatus::Committed) {
        error_message = "Cannot reconcile installed plugin packages: " +
                        format_filesystem_transaction_error(result);
        return false;
    }

    for (const PluginPackageTransactionEntry &entry : entries) {
        if (entry.action == PluginPackageTransactionAction::Replace)
            BOOST_LOG_TRIVIAL(info) << "Installed plugin package '" << entry.package_name << "' version '"
                                    << entry.version.package_version << "' for slicer '"
                                    << entry.version.slicer_version << "'.";
        else
            BOOST_LOG_TRIVIAL(info) << "Removed plugin package '" << entry.package_name << "'.";
    }

    // A successful commit makes any hidden transaction directories left by a
    // previously interrupted process obsolete. activated.ini is a regular
    // file and is never considered part of this package transaction.
    const boost::filesystem::path plugin_directory = data_directory / PLUGIN_DIRECTORY;
    cleanup_stale_plugin_transaction_directories(plugin_directory);
    return true;
}

bool request_plugin_install(const std::string &package_name,
                            const std::string &package_version,
                            const std::string &slicer_version,
                            std::string &error_message)
{
    if (!has_data_dir()) {
        error_message = "Cannot schedule a plugin installation before data_dir is available.";
        return false;
    }
    PluginInstalledVersion version{package_version, slicer_version};
    const boost::filesystem::path data_directory(data_dir());
    if (!prepare_plugin_cache(data_directory, error_message) ||
        !plugin_package_cache_is_valid(data_directory, package_name, version, error_message))
        return false;

    PluginActivationConfig config;
    bool from_user_config = false;
    if (!ensure_plugin_activation_config(data_directory, config, from_user_config, error_message))
        return false;
    config.installed[package_name] = version;
    return write_plugin_activation_config(plugin_activation_config_path(data_directory), config, error_message);
}

bool request_plugin_uninstall(const std::string &package_name, std::string &error_message)
{
    if (!has_data_dir()) {
        error_message = "Cannot schedule a plugin removal before data_dir is available.";
        return false;
    }
    if (!is_valid_plugin_package_name(package_name)) {
        error_message = "Cannot schedule removal for invalid plugin package '" + package_name + "'.";
        return false;
    }

    PluginActivationConfig config;
    bool from_user_config = false;
    const boost::filesystem::path data_directory(data_dir());
    if (!ensure_plugin_activation_config(data_directory, config, from_user_config, error_message))
        return false;
    config.installed.erase(package_name);
    for (std::map<std::string, std::string>::iterator it = config.plugin_packages.begin();
         it != config.plugin_packages.end();) {
        if (it->second != package_name) {
            ++it;
            continue;
        }
        config.activated.erase(it->first);
        it = config.plugin_packages.erase(it);
    }
    return write_plugin_activation_config(plugin_activation_config_path(data_directory), config, error_message);
}

} // namespace Slic3r
