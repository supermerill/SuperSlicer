///|/ Copyright (c) SuperSlicer 2026 Durand Remi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/

// This file implements the durable on-disk protocol shared by downloadable
// vendor and plugin packages. A package is always validated in a cache first;
// plugin packages are copied into the live directory only before DLL loading.

#include "PluginRepository.hpp"

#include <algorithm>
#include <cctype>
#include <exception>
#include <iterator>
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
#include "libslic3r/miniz_extension.hpp"
#include "libslic3r/Utils.hpp"

namespace Slic3r {
namespace {

const char *const PLUGIN_DIRECTORY = "plugins";
const char *const LEGACY_PLUGIN_DIRECTORY = "plugin";
const char *const ACTIVATED_PLUGINS_FILENAME = "activated.ini";
const char *const DEFAULT_ACTIVATED_PLUGINS_FILENAME = "default_activated.ini";
const char *const DESCRIPTION_FILENAME = "description.ini";

const char *plugin_package_library_filename();
bool ini_value_is_enabled(const std::string &value);
bool is_safe_package_name(const std::string &package_name);
bool is_four_component_version(const std::string &version);
bool parse_bundle_filename(const boost::filesystem::path &archive_path,
                           std::string &package_name,
                           PluginInstalledVersion &version);
bool is_safe_archive_entry(const std::string &entry_name);
bool copy_directory_tree(const boost::filesystem::path &source,
                         const boost::filesystem::path &destination,
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
boost::filesystem::path default_plugin_activation_config_path();

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

bool ini_value_is_enabled(const std::string &value)
{
    return boost::algorithm::iequals(value, "1") || boost::algorithm::iequals(value, "true") ||
           boost::algorithm::iequals(value, "yes") || boost::algorithm::iequals(value, "on") ||
           boost::algorithm::iequals(value, "enabled");
}

bool is_safe_package_name(const std::string &package_name)
{
    if (package_name.empty() || package_name.front() == '.' || package_name == "." || package_name == ".." ||
        package_name.find("..") != std::string::npos)
        return false;

    for (const unsigned char character : package_name)
        if (character > 0x7f || (!std::isalnum(character) && character != '.' && character != '_' && character != '-'))
            return false;
    return true;
}

bool is_four_component_version(const std::string &version)
{
    if (version.empty() || version.back() == '.')
        return false;

    size_t component_start = 0;
    unsigned int component_count = 0;
    while (component_start < version.size()) {
        const size_t component_end = version.find('.', component_start);
        const size_t end = component_end == std::string::npos ? version.size() : component_end;
        if (end == component_start)
            return false;
        for (size_t index = component_start; index < end; ++index)
            if (!std::isdigit(static_cast<unsigned char>(version[index])))
                return false;
        ++component_count;
        if (component_end == std::string::npos)
            break;
        component_start = component_end + 1;
    }
    return component_count == 4;
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
    return is_safe_package_name(package_name) && is_four_component_version(version.package_version) &&
           is_four_component_version(version.slicer_version);
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

bool validate_plugin_package(const boost::filesystem::path &package_root,
                             const std::string &package_name,
                             const PluginInstalledVersion &version,
                             std::string &error_message)
{
    if (!is_safe_package_name(package_name) || !is_four_component_version(version.package_version) ||
        !is_four_component_version(version.slicer_version)) {
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
    if (description.id != package_name || description.package_version != version.package_version ||
        description.slicer_version != version.slicer_version) {
        error_message = "Plugin description.ini does not match package '" + package_name + "' version '" +
                        version.package_version + "' for slicer '" + version.slicer_version + "'.";
        return false;
    }

    const boost::filesystem::path library_path = package_root / plugin_package_library_filename();
    if (!boost::filesystem::is_regular_file(library_path)) {
        error_message = "Plugin package '" + package_name + "' does not contain '" +
                        plugin_package_library_filename() + "' at its root.";
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

boost::filesystem::path default_plugin_activation_config_path()
{
    return boost::filesystem::path(resources_dir()) / PLUGIN_DIRECTORY / DEFAULT_ACTIVATED_PLUGINS_FILENAME;
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
        description.package_version = section.get<std::string>("package_version", section.get<std::string>("config_version", std::string()));
        description.slicer_version = section.get<std::string>("slicer_version", std::string());
        if (!is_safe_package_name(description.id)) {
            error_message = "Repository description contains an unsafe id '" + description.id + "'.";
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
    const char *directory_name = type == RepositoryPackageType::Vendor ? "vendor" : "plugins";
    return data_directory / "cache" / directory_name /
           (package_name + "_" + package_version + "_" + slicer_version);
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

boost::filesystem::path plugin_activation_config_path(const boost::filesystem::path &data_directory)
{
    return data_directory / PLUGIN_DIRECTORY / ACTIVATED_PLUGINS_FILENAME;
}

bool read_plugin_activation_config(const boost::filesystem::path &config_path,
                                   PluginActivationConfig &config,
                                   std::string &error_message)
{
    config = {};
    boost::nowide::ifstream stream(config_path.string());
    if (!stream) {
        error_message = "Cannot read plugin configuration '" + config_path.string() + "'.";
        return false;
    }

    try {
        boost::property_tree::ptree tree;
        boost::property_tree::read_ini(stream, tree);
        if (const boost::optional<boost::property_tree::ptree&> activated = tree.get_child_optional("activated"))
            for (const boost::property_tree::ptree::value_type &entry : *activated) {
                const std::string plugin_id = boost::algorithm::trim_copy(entry.first);
                if (!plugin_id.empty())
                    config.activated[plugin_id] = ini_value_is_enabled(entry.second.get_value<std::string>());
            }

        std::map<std::string, std::string> requested_slicer_versions;
        if (const boost::optional<boost::property_tree::ptree&> installed = tree.get_child_optional("installed"))
            for (const boost::property_tree::ptree::value_type &entry : *installed) {
                const std::string key = boost::algorithm::trim_copy(entry.first);
                const std::string value = boost::algorithm::trim_copy(entry.second.get_value<std::string>());
                const std::string suffix = ".slicer_version";
                if (key.size() > suffix.size() && key.compare(key.size() - suffix.size(), suffix.size(), suffix) == 0)
                    requested_slicer_versions.emplace(key.substr(0, key.size() - suffix.size()), value);
                else if (is_safe_package_name(key) && is_four_component_version(value))
                    config.installed[key].package_version = value;
                else
                    BOOST_LOG_TRIVIAL(warning) << "Ignoring invalid requested plugin package '" << key << "'.";
            }

        for (auto it = config.installed.begin(); it != config.installed.end();) {
            const std::map<std::string, std::string>::const_iterator slicer = requested_slicer_versions.find(it->first);
            // Older activated.ini files contained one version only. They were
            // produced while package and slicer versions were identical.
            it->second.slicer_version = slicer == requested_slicer_versions.end() ?
                it->second.package_version : slicer->second;
            if (!is_four_component_version(it->second.slicer_version)) {
                BOOST_LOG_TRIVIAL(warning) << "Ignoring invalid slicer version for plugin package '" << it->first << "'.";
                it = config.installed.erase(it);
            } else {
                ++it;
            }
        }

        if (const boost::optional<boost::property_tree::ptree&> removed = tree.get_child_optional("removed"))
            for (const boost::property_tree::ptree::value_type &entry : *removed) {
                const std::string package_name = boost::algorithm::trim_copy(entry.first);
                if (is_safe_package_name(package_name) &&
                    ini_value_is_enabled(entry.second.get_value<std::string>())) {
                    config.removed.insert(package_name);
                    // A removal request wins over a stale or manually merged
                    // installation request for the same package.
                    config.installed.erase(package_name);
                } else {
                    BOOST_LOG_TRIVIAL(warning) << "Ignoring invalid removed plugin package '" << package_name << "'.";
                }
            }
    } catch (const std::exception &error) {
        error_message = "Cannot parse plugin configuration '" + config_path.string() + "': " + error.what();
        return false;
    }
    return true;
}

bool write_plugin_activation_config(const boost::filesystem::path &config_path,
                                    const PluginActivationConfig &config,
                                    std::string &error_message)
{
    try {
        boost::filesystem::create_directories(config_path.parent_path());
        boost::nowide::ofstream stream(config_path.string(), std::ios::out | std::ios::trunc);
        if (!stream) {
            error_message = "Cannot write plugin configuration '" + config_path.string() + "'.";
            return false;
        }
        stream << "[installed]\n";
        for (const auto &[package_name, version] : config.installed) {
            stream << package_name << " = " << version.package_version << "\n";
            stream << package_name << ".slicer_version = " << version.slicer_version << "\n";
        }
        stream << "\n[removed]\n";
        for (const std::string &package_name : config.removed)
            stream << package_name << " = 1\n";
        stream << "\n[activated]\n";
        for (const auto &[plugin_id, enabled] : config.activated)
            stream << plugin_id << " = " << (enabled ? "1" : "0") << "\n";
        if (!stream.good()) {
            error_message = "Cannot finish writing plugin configuration '" + config_path.string() + "'.";
            return false;
        }
    } catch (const boost::filesystem::filesystem_error &error) {
        error_message = error.what();
        return false;
    }
    return true;
}

bool ensure_plugin_activation_config(const boost::filesystem::path &data_directory,
                                     PluginActivationConfig &config,
                                     bool &from_user_config,
                                     std::string &error_message)
{
    from_user_config = false;
    if (data_directory.empty())
        return read_plugin_activation_config(default_plugin_activation_config_path(), config, error_message);

    const boost::filesystem::path config_path = plugin_activation_config_path(data_directory);
    const boost::filesystem::path legacy_config_path = data_directory / LEGACY_PLUGIN_DIRECTORY / ACTIVATED_PLUGINS_FILENAME;
    try {
        if (!boost::filesystem::exists(config_path)) {
            boost::filesystem::create_directories(config_path.parent_path());
            if (boost::filesystem::exists(legacy_config_path))
                boost::filesystem::copy_file(legacy_config_path, config_path);
            else
                boost::filesystem::copy_file(default_plugin_activation_config_path(), config_path);
        }
    } catch (const boost::filesystem::filesystem_error &error) {
        error_message = "Cannot prepare plugin configuration '" + config_path.string() + "': " + error.what();
        return false;
    }
    from_user_config = true;
    return read_plugin_activation_config(config_path, config, error_message);
}

bool cache_plugin_package_archive(const boost::filesystem::path &data_directory,
                                  const boost::filesystem::path &archive_path,
                                  const std::string &package_name,
                                  const std::string &package_version,
                                  const std::string &slicer_version,
                                  std::string &error_message)
{
    const PluginInstalledVersion version{package_version, slicer_version};
    const boost::filesystem::path cache_root = repository_package_cache_path(
        data_directory, RepositoryPackageType::Plugin, package_name, package_version, slicer_version);
    try {
        if (boost::filesystem::is_directory(cache_root)) {
            std::string validation_error;
            if (validate_plugin_package(cache_root, package_name, version, validation_error))
                return true;
            BOOST_LOG_TRIVIAL(warning) << validation_error << " Re-extracting plugin archive.";
            boost::filesystem::remove_all(cache_root);
        }

        const boost::filesystem::path staging = cache_root.parent_path() /
            boost::filesystem::unique_path("." + package_name + ".extract-%%%%-%%%%");
        boost::filesystem::create_directories(cache_root.parent_path());
        if (!extract_repository_archive(archive_path, staging, error_message)) {
            boost::filesystem::remove_all(staging);
            return false;
        }

        boost::filesystem::path package_root = staging;
        if (!boost::filesystem::exists(package_root / DESCRIPTION_FILENAME)) {
            boost::filesystem::directory_iterator it(staging);
            const boost::filesystem::directory_iterator end;
            if (it == end || !boost::filesystem::is_directory(it->path())) {
                boost::filesystem::remove_all(staging);
                error_message = "Plugin archive '" + archive_path.string() + "' has no package root.";
                return false;
            }
            package_root = it->path();
            ++it;
            if (it != end) {
                boost::filesystem::remove_all(staging);
                error_message = "Plugin archive '" + archive_path.string() + "' has more than one root entry.";
                return false;
            }
        }
        if (!validate_plugin_package(package_root, package_name, version, error_message)) {
            boost::filesystem::remove_all(staging);
            return false;
        }
        if (package_root == staging) {
            boost::filesystem::rename(staging, cache_root);
        } else {
            boost::filesystem::rename(package_root, cache_root);
            boost::filesystem::remove_all(staging);
        }
    } catch (const boost::filesystem::filesystem_error &error) {
        error_message = "Cannot cache plugin package '" + package_name + "': " + error.what();
        return false;
    }
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

bool prepare_plugin_bundle_cache(const boost::filesystem::path &resources_directory,
                                 const boost::filesystem::path &data_directory,
                                 std::string &error_message)
{
    const boost::filesystem::path bundles_directory = resources_directory / PLUGIN_DIRECTORY;
    if (!boost::filesystem::is_directory(bundles_directory))
        return true;

    try {
        for (boost::filesystem::directory_iterator it(bundles_directory), end; it != end; ++it) {
            if (!boost::filesystem::is_regular_file(it->path()))
                continue;
            std::string package_name;
            PluginInstalledVersion version;
            if (!parse_bundle_filename(it->path(), package_name, version)) {
                if (boost::algorithm::iequals(it->path().extension().string(), ".zip"))
                    BOOST_LOG_TRIVIAL(warning) << "Ignoring invalid plugin archive name '" << it->path().filename().string() << "'.";
                continue;
            }

            if (!cache_plugin_package_archive(data_directory, it->path(), package_name, version.package_version,
                                              version.slicer_version, error_message))
                return false;
        }
    } catch (const boost::filesystem::filesystem_error &error) {
        error_message = "Cannot prepare plugin bundle cache: " + std::string(error.what());
        return false;
    }
    return true;
}

bool apply_requested_plugin_package_changes(const boost::filesystem::path &data_directory,
                                            PluginActivationConfig &config,
                                            std::vector<std::string> &warnings,
                                            std::string &error_message)
{
    warnings.clear();
    error_message.clear();

    // Validate every requested installation before changing a live package.
    // A missing archive therefore cannot leave a partially applied startup.
    for (const auto &[package_name, version] : config.installed)
        if (!plugin_package_cache_is_valid(data_directory, package_name, version, error_message))
            return false;

    for (const auto &[package_name, version] : config.installed)
        if (!replace_installed_package(data_directory, package_name, version, error_message))
            return false;

    PluginActivationConfig updated_config = config;
    for (const std::string &package_name : config.removed) {
        const boost::filesystem::path package_root = data_directory / PLUGIN_DIRECTORY / package_name;
        try {
            if (!boost::filesystem::exists(package_root)) {
                warnings.emplace_back("Plugin package '" + package_name + "' was already absent from the live plugin directory.");
            } else {
                boost::filesystem::remove_all(package_root);
            }
        } catch (const boost::filesystem::filesystem_error &error) {
            error_message = "Cannot remove plugin package '" + package_name + "': " + error.what();
            return false;
        }
        updated_config.removed.erase(package_name);
    }

    // Removal markers are one-shot startup requests. Persist their consumption
    // only after every requested filesystem operation has completed.
    if (updated_config.removed != config.removed) {
        if (!write_plugin_activation_config(plugin_activation_config_path(data_directory), updated_config, error_message))
            return false;
        config = std::move(updated_config);
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
    if (!prepare_plugin_bundle_cache(boost::filesystem::path(resources_dir()), data_directory, error_message) ||
        !plugin_package_cache_is_valid(data_directory, package_name, version, error_message))
        return false;

    PluginActivationConfig config;
    bool from_user_config = false;
    if (!ensure_plugin_activation_config(data_directory, config, from_user_config, error_message))
        return false;
    config.removed.erase(package_name);
    config.installed[package_name] = version;
    return write_plugin_activation_config(plugin_activation_config_path(data_directory), config, error_message);
}

bool request_plugin_uninstall(const std::string &package_name, std::string &error_message)
{
    if (!has_data_dir()) {
        error_message = "Cannot schedule a plugin removal before data_dir is available.";
        return false;
    }
    if (!is_safe_package_name(package_name)) {
        error_message = "Cannot schedule removal for invalid plugin package '" + package_name + "'.";
        return false;
    }

    PluginActivationConfig config;
    bool from_user_config = false;
    const boost::filesystem::path data_directory(data_dir());
    if (!ensure_plugin_activation_config(data_directory, config, from_user_config, error_message))
        return false;
    config.installed.erase(package_name);
    config.removed.insert(package_name);
    return write_plugin_activation_config(plugin_activation_config_path(data_directory), config, error_message);
}

} // namespace Slic3r
