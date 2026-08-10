///|/ Copyright (c) SuperSlicer 2026 Durand Remi @supermerill
///|/
///|/ SuperSlicer is released under the terms of the AGPLv3 or higher
///|/

// This file owns the on-disk lifecycle of external plugin packages. Bundles
// distributed with the application are immutable ZIP files. They are first
// unpacked into a versioned cache and only copied into the user plugin folder
// on a later launch, before any external library can be loaded.

#include "PluginRepository.hpp"

#include <cctype>
#include <exception>
#include <sstream>
#include <string>

#include <boost/algorithm/string/predicate.hpp>
#include <boost/algorithm/string/trim.hpp>
#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>
#include <boost/nowide/fstream.hpp>
#include <boost/property_tree/ini_parser.hpp>
#include <boost/property_tree/ptree.hpp>

#include "libslic3r/miniz_extension.hpp"
#include "libslic3r/Utils.hpp"

namespace Slic3r {
namespace {

const char *const PLUGIN_DIRECTORY = "plugins";
const char *const LEGACY_PLUGIN_DIRECTORY = "plugin";
const char *const PLUGIN_CACHE_DIRECTORY = "cache/plugins";
const char *const ACTIVATED_PLUGINS_FILENAME = "activated.ini";
const char *const DEFAULT_ACTIVATED_PLUGINS_FILENAME = "default_activated.ini";

const char *plugin_package_library_filename();
bool ini_value_is_enabled(const std::string &value);
bool is_safe_package_name(const std::string &package_name);
bool is_four_component_version(const std::string &version);
bool parse_bundle_filename(const boost::filesystem::path &archive_path,
                           std::string &package_name,
                           std::string &version);
bool validate_manifest(const boost::filesystem::path &manifest_path,
                       const std::string &package_name,
                       const std::string &version,
                       std::string &error_message);
bool write_local_manifest(const boost::filesystem::path &manifest_path,
                    const std::string &package_name,
                    const std::string &version,
                    std::string &error_message);
bool ensure_package_manifest(const boost::filesystem::path &package_root,
                             const std::string &package_name,
                             const std::string &version,
                             std::string &error_message);
bool validate_package_contents(const boost::filesystem::path &package_root,
                               const std::string &package_name,
                               const std::string &version,
                               std::string &error_message);
bool is_safe_archive_entry(const std::string &entry_name);
bool extract_bundle_to_directory(const boost::filesystem::path &archive_path,
                                 const boost::filesystem::path &destination,
                                 std::string &error_message);
bool copy_directory_tree(const boost::filesystem::path &source,
                         const boost::filesystem::path &destination,
                         std::string &error_message);
bool cached_package_version_is_valid(const boost::filesystem::path &data_directory,
                                     const std::string &package_name,
                                     const std::string &version,
                                     std::string &error_message);
bool installed_package_matches(const boost::filesystem::path &package_root,
                               const std::string &package_name,
                               const std::string &version);
bool replace_installed_package(const boost::filesystem::path &data_directory,
                               const std::string &package_name,
                               const std::string &version,
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
    return boost::algorithm::iequals(value, "1") ||
           boost::algorithm::iequals(value, "true") ||
           boost::algorithm::iequals(value, "yes") ||
           boost::algorithm::iequals(value, "on") ||
           boost::algorithm::iequals(value, "enabled");
}

bool is_safe_package_name(const std::string &package_name)
{
    if (package_name.empty() || package_name.front() == '.' || package_name == "." || package_name == ".." ||
        package_name.find("..") != std::string::npos)
        return false;

    for (const unsigned char character : package_name) {
        if (character > 0x7f ||
            (!std::isalnum(character) && character != '.' && character != '_' && character != '-'))
            return false;
    }
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
                           std::string &version)
{
    if (!boost::algorithm::iequals(archive_path.extension().string(), ".zip"))
        return false;

    const std::string stem = archive_path.stem().string();
    const size_t separator = stem.rfind('_');
    if (separator == std::string::npos || separator == 0 || separator + 1 == stem.size())
        return false;

    package_name = stem.substr(0, separator);
    version = stem.substr(separator + 1);
    return is_safe_package_name(package_name) && is_four_component_version(version);
}

bool validate_manifest(const boost::filesystem::path &manifest_path,
                       const std::string &package_name,
                       const std::string &version,
                       std::string &error_message)
{
    boost::nowide::ifstream stream(manifest_path.string());
    if (!stream) {
        error_message = "Cannot read plugin manifest '" + manifest_path.string() + "'.";
        return false;
    }

    boost::property_tree::ptree tree;
    try {
        boost::property_tree::read_ini(stream, tree);
        const std::string manifest_id = tree.get<std::string>("plugin.id");
        const std::string manifest_type = tree.get<std::string>("plugin.type");
        const std::string manifest_version = tree.get<std::string>("plugin.version");
        if (manifest_id != package_name || manifest_type != "local" || manifest_version != version) {
            error_message = "Plugin manifest '" + manifest_path.string() + "' does not match package '" +
                            package_name + "' version '" + version + "'.";
            return false;
        }
    } catch (const std::exception &error) {
        error_message = "Cannot parse plugin manifest '" + manifest_path.string() + "': " + error.what();
        return false;
    }
    return true;
}

bool write_local_manifest(const boost::filesystem::path &manifest_path,
                    const std::string &package_name,
                    const std::string &version,
                    std::string &error_message)
{
    boost::nowide::ofstream stream(manifest_path.string(), std::ios::out | std::ios::trunc);
    if (!stream) {
        error_message = "Cannot write plugin manifest '" + manifest_path.string() + "'.";
        return false;
    }
    stream << "[plugin]\n"
           << "id = " << package_name << "\n"
           << "type = local\n"
           << "version = " << version << "\n";
    return stream.good();
}

bool ensure_package_manifest(const boost::filesystem::path &package_root,
                             const std::string &package_name,
                             const std::string &version,
                             std::string &error_message)
{
    const boost::filesystem::path manifest_path = package_root / (package_name + ".ini");
    if (boost::filesystem::exists(manifest_path))
        return validate_manifest(manifest_path, package_name, version, error_message);
    return write_local_manifest(manifest_path, package_name, version, error_message);
}

bool validate_package_contents(const boost::filesystem::path &package_root,
                               const std::string &package_name,
                               const std::string &version,
                               std::string &error_message)
{
    if (!ensure_package_manifest(package_root, package_name, version, error_message))
        return false;

    const boost::filesystem::path library_path = package_root / plugin_package_library_filename();
    if (!boost::filesystem::is_regular_file(library_path)) {
        error_message = "Plugin package '" + package_name + "' version '" + version +
                        "' does not contain '" + plugin_package_library_filename() + "' at its root.";
        return false;
    }
    return true;
}

bool is_safe_archive_entry(const std::string &entry_name)
{
    if (entry_name.empty() || entry_name.front() == '/' || entry_name.front() == '\\' ||
        entry_name.find(':') != std::string::npos)
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
    while (std::getline(components, component, '/')) {
        if (component.empty() || component == "." || component == "..")
            return false;
    }
    return true;
}

bool extract_bundle_to_directory(const boost::filesystem::path &archive_path,
                                 const boost::filesystem::path &destination,
                                 std::string &error_message)
{
    ZipReader archive(archive_path.string());
    if (!archive.success()) {
        error_message = "Cannot open plugin archive '" + archive_path.string() + "'.";
        return false;
    }

    const mz_uint entry_count = mz_zip_reader_get_num_files(&archive.archive);
    for (mz_uint index = 0; index < entry_count; ++index) {
        mz_zip_archive_file_stat entry = {};
        if (!mz_zip_reader_file_stat(&archive.archive, index, &entry) || !is_safe_archive_entry(entry.m_filename)) {
            error_message = "Plugin archive '" + archive_path.string() + "' contains an unsafe entry.";
            return false;
        }

        const std::string entry_name(entry.m_filename);
        const bool is_directory = !entry_name.empty() &&
                                  (entry_name.back() == '/' || entry_name.back() == '\\');
        const boost::filesystem::path output_path = destination / boost::filesystem::path(entry_name);
        if (is_directory) {
            boost::filesystem::create_directories(output_path);
            continue;
        }

        boost::filesystem::create_directories(output_path.parent_path());
        if (!mz_zip_reader_extract_to_file(&archive.archive, entry.m_file_index, output_path.string().c_str(), 0)) {
            error_message = "Cannot extract '" + entry_name + "' from plugin archive '" + archive_path.string() + "'.";
            return false;
        }
    }
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
                error_message = "Plugin package '" + source.string() + "' contains an unsupported filesystem entry.";
                return false;
            }
        }
    } catch (const boost::filesystem::filesystem_error &error) {
        error_message = error.what();
        return false;
    }
    return true;
}

bool cached_package_version_is_valid(const boost::filesystem::path &data_directory,
                                     const std::string &package_name,
                                     const std::string &version,
                                     std::string &error_message)
{
    if (!is_safe_package_name(package_name) || !is_four_component_version(version)) {
        error_message = "Invalid plugin package name or version.";
        return false;
    }
    const boost::filesystem::path package_root = data_directory / PLUGIN_CACHE_DIRECTORY / package_name / version;
    if (!boost::filesystem::is_directory(package_root)) {
        error_message = "Plugin package '" + package_name + "' version '" + version + "' is not cached.";
        return false;
    }
    return validate_package_contents(package_root, package_name, version, error_message);
}

bool installed_package_matches(const boost::filesystem::path &package_root,
                               const std::string &package_name,
                               const std::string &version)
{
    std::string ignored_error;
    return boost::filesystem::is_directory(package_root) &&
           validate_package_contents(package_root, package_name, version, ignored_error);
}

bool replace_installed_package(const boost::filesystem::path &data_directory,
                               const std::string &package_name,
                               const std::string &version,
                               std::string &error_message)
{
    if (!cached_package_version_is_valid(data_directory, package_name, version, error_message))
        return false;

    const boost::filesystem::path source = data_directory / PLUGIN_CACHE_DIRECTORY / package_name / version;
    const boost::filesystem::path plugin_directory = data_directory / PLUGIN_DIRECTORY;
    const boost::filesystem::path destination = plugin_directory / package_name;
    if (installed_package_matches(destination, package_name, version))
        return true;

    const boost::filesystem::path staging = plugin_directory / boost::filesystem::unique_path("." + package_name + ".install-%%%%-%%%%");
    const boost::filesystem::path backup = plugin_directory / boost::filesystem::unique_path("." + package_name + ".backup-%%%%-%%%%");
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

    BOOST_LOG_TRIVIAL(info) << "Installed plugin package '" << package_name << "' version '" << version << "'.";
    return true;
}

boost::filesystem::path default_plugin_activation_config_path()
{
    return boost::filesystem::path(resources_dir()) / PLUGIN_DIRECTORY / DEFAULT_ACTIVATED_PLUGINS_FILENAME;
}

} // namespace

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

    boost::property_tree::ptree tree;
    try {
        boost::property_tree::read_ini(stream, tree);
        if (const boost::optional<const boost::property_tree::ptree&> activated = tree.get_child_optional("activated"))
            for (const boost::property_tree::ptree::value_type &entry : *activated) {
                const std::string plugin_id = boost::algorithm::trim_copy(entry.first);
                if (!plugin_id.empty())
                    config.activated[plugin_id] = ini_value_is_enabled(entry.second.get_value<std::string>());
            }
        if (const boost::optional<const boost::property_tree::ptree&> installed = tree.get_child_optional("installed"))
            for (const boost::property_tree::ptree::value_type &entry : *installed) {
                const std::string package_name = boost::algorithm::trim_copy(entry.first);
                const std::string version = boost::algorithm::trim_copy(entry.second.get_value<std::string>());
                if (is_safe_package_name(package_name) && is_four_component_version(version))
                    config.installed[package_name] = version;
                else
                    BOOST_LOG_TRIVIAL(warning) << "Ignoring invalid requested plugin package '" << package_name << "'.";
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
        for (const auto &[package_name, version] : config.installed)
            stream << package_name << " = " << version << "\n";
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
            std::string version;
            if (!parse_bundle_filename(it->path(), package_name, version)) {
                if (boost::algorithm::iequals(it->path().extension().string(), ".zip"))
                    BOOST_LOG_TRIVIAL(warning) << "Ignoring invalid plugin archive name '" << it->path().filename().string() << "'.";
                continue;
            }

            const boost::filesystem::path cache_root = data_directory / PLUGIN_CACHE_DIRECTORY / package_name / version;
            if (boost::filesystem::is_directory(cache_root)) {
                std::string manifest_error;
                if (validate_package_contents(cache_root, package_name, version, manifest_error))
                    continue;
                BOOST_LOG_TRIVIAL(warning) << manifest_error << " Re-extracting plugin archive.";
                boost::filesystem::remove_all(cache_root);
            }

            const boost::filesystem::path cache_parent = cache_root.parent_path();
            const boost::filesystem::path staging = cache_parent / boost::filesystem::unique_path("." + version + ".extract-%%%%-%%%%");
            boost::filesystem::create_directories(cache_parent);
            if (!extract_bundle_to_directory(it->path(), staging, error_message) ||
                !validate_package_contents(staging, package_name, version, error_message)) {
                boost::filesystem::remove_all(staging);
                return false;
            }
            boost::filesystem::rename(staging, cache_root);
            BOOST_LOG_TRIVIAL(info) << "Cached plugin package '" << package_name << "' version '" << version << "'.";
        }
    } catch (const boost::filesystem::filesystem_error &error) {
        error_message = "Cannot prepare plugin bundle cache: " + std::string(error.what());
        return false;
    }
    return true;
}

bool install_requested_plugin_packages(const boost::filesystem::path &data_directory,
                                       const PluginActivationConfig &config,
                                       std::string &error_message)
{
    for (const auto &[package_name, version] : config.installed)
        if (!replace_installed_package(data_directory, package_name, version, error_message))
            return false;
    return true;
}

bool request_plugin_install(const std::string &package_name,
                            const std::string &version,
                            std::string &error_message)
{
    if (!has_data_dir()) {
        error_message = "Cannot schedule a plugin installation before data_dir is available.";
        return false;
    }

    const boost::filesystem::path data_directory(data_dir());
    if (!prepare_plugin_bundle_cache(boost::filesystem::path(resources_dir()), data_directory, error_message) ||
        !cached_package_version_is_valid(data_directory, package_name, version, error_message))
        return false;

    PluginActivationConfig config;
    bool from_user_config = false;
    if (!ensure_plugin_activation_config(data_directory, config, from_user_config, error_message))
        return false;

    config.installed[package_name] = version;
    return write_plugin_activation_config(plugin_activation_config_path(data_directory), config, error_message);
}

} // namespace Slic3r
