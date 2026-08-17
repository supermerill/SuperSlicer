///|/ Copyright (c) SuperSlicer 2026 Durand Remi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/

// This file implements the durable on-disk protocol shared by downloadable
// vendor and plugin packages. A package is always validated in a cache first;
// plugin packages are copied into the live directory only before native or
// Python plugin loading begins.

#include "PluginRepository.hpp"

#include <algorithm>
#include <exception>
#include <iterator>
#include <map>
#include <sstream>
#include <string>

#include <boost/algorithm/string/predicate.hpp>
#include <boost/algorithm/string/trim.hpp>
#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>
#include <boost/nowide/fstream.hpp>
#include <boost/property_tree/ini_parser.hpp>
#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ptree.hpp>

#include "libslic3r/Semver.hpp"
#include "libslic3r/Updater/RepositoryPackageCache.hpp"
#include "libslic3r/miniz_extension.hpp"
#include "libslic3r/Utils.hpp"

namespace Slic3r {
namespace {

const char *const PLUGIN_DIRECTORY = "plugins";
const char *const DESCRIPTION_FILENAME = "description.ini";
const char *const VERSION_FILENAME = "version.ini";

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
bool replace_installed_package(const boost::filesystem::path &data_directory,
                               const std::string &package_name,
                               const PluginInstalledVersion &version,
                               std::string &error_message);

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

bool replace_installed_package(const boost::filesystem::path &data_directory,
                               const std::string &package_name,
                               const PluginInstalledVersion &version,
                               std::string &error_message)
{
    if (!plugin_package_cache_is_valid(data_directory, package_name, version, error_message))
        return false;

    const boost::filesystem::path source = repository_package_cache_path(
        data_directory, RepositoryPackageType::Plugin, package_name, version.package_version, version.slicer_version);
    const boost::filesystem::path plugin_directory = data_directory / PLUGIN_DIRECTORY;
    const boost::filesystem::path destination = plugin_directory / package_name;
    if (installed_package_matches(destination, package_name, version))
        return true;

    const boost::filesystem::path staging = plugin_directory /
        boost::filesystem::unique_path("." + package_name + ".install-%%%%-%%%%");
    const boost::filesystem::path backup = plugin_directory /
        boost::filesystem::unique_path("." + package_name + ".backup-%%%%-%%%%");
    try {
        boost::filesystem::create_directories(plugin_directory);
        if (!copy_directory_tree(source, staging, error_message)) {
            boost::filesystem::remove_all(staging);
            return false;
        }

        const bool had_previous_installation = boost::filesystem::exists(destination);
        if (had_previous_installation)
            boost::filesystem::rename(destination, backup);
        try {
            boost::filesystem::rename(staging, destination);
        } catch (const boost::filesystem::filesystem_error &) {
            if (had_previous_installation && !boost::filesystem::exists(destination))
                boost::filesystem::rename(backup, destination);
            throw;
        }
        if (had_previous_installation)
            boost::filesystem::remove_all(backup);
    } catch (const boost::filesystem::filesystem_error &error) {
        boost::filesystem::remove_all(staging);
        error_message = "Cannot install plugin package '" + package_name + "': " + error.what();
        return false;
    }

    BOOST_LOG_TRIVIAL(info) << "Installed plugin package '" << package_name << "' version '"
                            << version.package_version << "' for slicer '" << version.slicer_version << "'.";
    return true;
}

} // namespace

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

    // Validate every requested installation before changing a live package.
    // A missing archive therefore cannot leave a partially applied startup.
    for (const auto &[package_name, version] : config.installed)
        if (!plugin_package_cache_is_valid(data_directory, package_name, version, error_message))
            return false;

    for (const auto &[package_name, version] : config.installed)
        if (!replace_installed_package(data_directory, package_name, version, error_message))
            return false;

    // Every remaining direct subdirectory is a package that is no longer in
    // the desired set. Files such as activated.ini are deliberately ignored.
    const boost::filesystem::path plugin_directory = data_directory / PLUGIN_DIRECTORY;
    try {
        std::vector<boost::filesystem::path> packages_to_remove;
        if (boost::filesystem::is_directory(plugin_directory)) {
            for (boost::filesystem::directory_iterator it(plugin_directory), end; it != end; ++it) {
                if (!boost::filesystem::is_directory(it->path()))
                    continue;
                const std::string package_name = it->path().filename().string();
                if (config.installed.count(package_name) == 0)
                    packages_to_remove.emplace_back(it->path());
            }
        }

        // Finish directory enumeration before deleting entries. Removing the
        // current entry may invalidate platform-specific directory iterators.
        for (const boost::filesystem::path &package_path : packages_to_remove)
            boost::filesystem::remove_all(package_path);
    } catch (const boost::filesystem::filesystem_error &error) {
        error_message = "Cannot reconcile live plugin packages: " + std::string(error.what());
        return false;
    }
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
